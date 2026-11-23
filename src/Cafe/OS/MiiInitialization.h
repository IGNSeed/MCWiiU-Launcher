#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

struct PPCInterpreter_t;

namespace MiiInitialization
{

constexpr int32_t BoundaryExitStatus = 0x4d4949;
enum class Outcome { None, Waiting, Boundary, Rejected };
struct Result
{
	Outcome outcome = Outcome::None;
	uint64_t generation = 0;
	uint32_t fflResult = ~0u;
	uint32_t manager = 0;
};

inline std::atomic_bool s_requested{false}, s_observing{false};
inline bool IsRequested() { return s_requested.load(std::memory_order_acquire); }
inline bool IsObserving() { return s_observing.load(std::memory_order_relaxed); }

// coordinatorのみ。検証済みRPXからの専用セッションに限定し、永続設定は変更しない。
bool SupportsRpx(std::string_view rpxSha256);
bool Request(std::string_view rpxSha256);
void ValidateRpx(std::string_view virtualPath);
void BindTitle(uint64_t generation);
void ValidateLoadedImage();
void BeforeInstruction(PPCInterpreter_t* cpu);
// scheduler join後に解除する。結果はcoordinatorが消費するまで保持する。
void EndTitle();
Result GetResult();

} // namespace MiiInitialization
