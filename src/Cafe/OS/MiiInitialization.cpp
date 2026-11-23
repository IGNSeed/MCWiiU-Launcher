#include "MiiInitialization.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "config/ActiveSettings.h"
#include "Cafe/Filesystem/fsc.h"
#include <openssl/sha.h>

namespace coreinit { void __PPCExit(sint32 status); }

namespace
{
	// このdescriptorは同一SHAのJPN v560だけに適用する。別RPXへ推測適用しない。
	struct Descriptor
	{
		std::string_view sha256;
		uint32_t entry, bootReturn, fflReturn, forbidden, manager;
	};
	constexpr Descriptor s_jpn{
		"d03d35636eb148df52f58c6fd680409103b612a23988ed0001d8509e486c28b8",
		0x03340a80, 0x033e51cc, 0x03340ad4, 0x032fba00, 0x1048d4c8};
	std::mutex s_mutex;
	MiiInitialization::Result s_result;
	uint32_t s_thread = 0, s_bootResult = ~0u;
	bool s_entered = false, s_bootReturned = false;

	void StopAtBoundary(bool accepted)
	{
		// guestのr3/LRや元命令には触れず、世代付きの既存終了経路を使用する。
		coreinit::__PPCExit(accepted ? MiiInitialization::BoundaryExitStatus : -MiiInitialization::BoundaryExitStatus);
		// 予期せぬresumeでも後続のプロフィール処理へ戻さない。
		while (true) coreinit::OSSuspendThread(coreinit::OSGetCurrentThread());
	}
}

namespace MiiInitialization
{

bool SupportsRpx(std::string_view rpxSha256) { return rpxSha256 == s_jpn.sha256; }

void ValidateRpx(std::string_view virtualPath)
{
	if (!IsRequested()) return;
	const auto file = fsc_extractFile(std::string(virtualPath).c_str());
	std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
	std::string hash;
	if (file)
	{
		SHA256(file->data(), file->size(), digest.data());
		for (auto byte : digest) hash += fmt::format("{:02x}", byte);
	}
	if (!SupportsRpx(hash))
	{
		std::lock_guard lock(s_mutex);
		s_result.outcome = Outcome::Rejected;
		throw std::runtime_error("Mii initialization executable identity mismatch");
	}
}

bool Request(std::string_view rpxSha256)
{
	std::lock_guard lock(s_mutex);
	if (rpxSha256 != s_jpn.sha256 || IsRequested() || CafeSystem::IsTitleRunning()) return false;
	s_result = {Outcome::Waiting};
	s_thread = 0;
	s_bootResult = ~0u;
	s_entered = s_bootReturned = false;
	s_requested.store(true, std::memory_order_release);
	return true;
}

void BindTitle(uint64_t generation)
{
	std::lock_guard lock(s_mutex);
	if (!IsRequested() || !generation || s_result.generation) throw std::runtime_error("Invalid Mii initialization session");
	s_result.generation = generation;
}

void ValidateLoadedImage()
{
	if (!IsRequested()) return;
	std::lock_guard lock(s_mutex);
	if (CafeSystem::GetForegroundTitleId() != 0x00050000101dbe00ULL || CafeSystem::GetForegroundTitleVersion() != 560 ||
		CafeSystem::GetTitleGeneration() != s_result.generation || ActiveSettings::GetCPUMode() != CPUMode::SinglecoreInterpreter ||
		memory_readU32(s_jpn.entry) != 0x7c0802a6 || memory_readU32(s_jpn.bootReturn) != 0x2c030000 ||
		memory_readU32(s_jpn.fflReturn) != 0x2c030000 || memory_readU32(s_jpn.forbidden) != 0xa0fa001e)
	{
		s_result.outcome = Outcome::Rejected;
		throw std::runtime_error("Mii initialization RPX instructions do not match the verified descriptor");
	}
	s_observing.store(true, std::memory_order_release);
	cemuLog_log(LogType::Force, "Mii initialization boundaries armed: generation={}", s_result.generation);
}

void BeforeInstruction(PPCInterpreter_t* cpu)
{
	const auto ip = cpu->instructionPointer;
	if (ip != s_jpn.entry && ip != s_jpn.bootReturn && ip != s_jpn.fflReturn && ip != s_jpn.forbidden) return;
	bool stop = false, accepted = false;
	{
		std::lock_guard lock(s_mutex);
		if (!IsObserving()) return;
		const auto thread = MEMPTR<OSThread_t>(coreinit::OSGetCurrentThread()).GetMPTR();
		const bool current = s_result.generation == CafeSystem::GetTitleGeneration();
		if (current && ip == s_jpn.entry && !s_entered && memory_readU32(ip) == 0x7c0802a6)
		{
			s_entered = true;
			s_thread = thread;
			cemuLog_log(LogType::Force, "Mii initialization FFL entry: generation={}", s_result.generation);
			return;
		}
		if (current && s_entered && thread == s_thread && ip == s_jpn.bootReturn && !s_bootReturned && memory_readU32(ip) == 0x2c030000)
		{
			s_bootReturned = true;
			s_bootResult = cpu->gpr[3];
			return;
		}
		s_result.fflResult = cpu->gpr[3];
		s_result.manager = memory_readU32(s_jpn.manager);
		accepted = current && s_entered && s_bootReturned && thread == s_thread && ip == s_jpn.fflReturn &&
			memory_readU32(ip) == 0x2c030000 && s_bootResult == s_result.fflResult &&
			(s_result.fflResult == 0 || s_result.fflResult == 10) && s_result.manager != 0;
		s_result.outcome = accepted ? Outcome::Boundary : Outcome::Rejected;
		// one-shot。停止要求後は同じ境界を再処理しない。
		s_observing.store(false, std::memory_order_release);
		stop = true;
		cemuLog_log(LogType::Force, "Mii initialization boundary: accepted={} result={} generation={}", accepted, s_result.fflResult, s_result.generation);
	}
	if (stop) StopAtBoundary(accepted);
}

void EndTitle()
{
	s_observing.store(false, std::memory_order_release);
	s_requested.store(false, std::memory_order_release);
}

Result GetResult()
{
	std::lock_guard lock(s_mutex);
	return s_result;
}

} // namespace MiiInitialization
