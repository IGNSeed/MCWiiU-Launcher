#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/MMU/MMU.h"
#include "input/GuestInput.h"
#include "mcwiiu_input.h"
#ifdef _WIN32
#include <Windows.h>
#endif

namespace guestinput
{
	namespace
	{
		std::span<uint8_t> Output(uint32 address, uint32 bytes, uint32 required)
		{
			if (!Provider::BufferRange(address, bytes, required))
				return {};
			// 出力先はmap済みのGuest data RAMだけ。code、private/system、MMIOは受け付けない。
			const bool ram = (mmuRange_MEM2.isMapped() && address >= mmuRange_MEM2.getBase() &&
							  static_cast<uint64>(address) + bytes <= mmuRange_MEM2.getEnd()) ||
							 (mmuRange_MEM1.isMapped() && address >= mmuRange_MEM1.getBase() &&
							  static_cast<uint64>(address) + bytes <= mmuRange_MEM1.getEnd());
			if (!ram || !memory_isAddressRangeAccessible(address, bytes))
				return {};
			auto* pointer = memory_getPointerFromVirtualOffset(address);
#ifdef _WIN32
			for (uintptr_t cursor = reinterpret_cast<uintptr_t>(pointer), end = cursor + bytes; cursor < end;)
			{
				MEMORY_BASIC_INFORMATION region{};
				if (VirtualQuery(reinterpret_cast<void*>(cursor), &region, sizeof(region)) != sizeof(region) ||
					region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
					!(region.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
					return {};
				cursor = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
			}
#endif
			return {pointer, bytes};
		}

		uint32 MCWInputPoll(uint32 session, uint32 consumer, uint32 expectedResetSerial, uint32 expectedCaptureSequence)
		{
			return Get().Poll(session, consumer, expectedResetSerial, expectedCaptureSequence);
		}

		uint32 MCWInputToggle(uint32 session, uint32 consumer, uint32 enabled, uint32 output, uint32 bytes)
		{
			return Get().Toggle(session, consumer, enabled, Output(output, bytes, MCW_INPUT_HEADER_SIZE));
		}

		uint32 MCWInputQuery(uint32 output, uint32 bytes)
		{
			return Get().Query(Output(output, bytes, MCW_INPUT_HEADER_SIZE));
		}

		uint32 MCWInputRegister(uint32 major, uint32 output, uint32 bytes)
		{
			return Get().Register(major, Output(output, bytes, MCW_INPUT_HEADER_SIZE));
		}

		uint32 MCWInputUnregister(uint32 session, uint32 consumer)
		{
			return Get().Unregister(session, consumer);
		}

		uint32 MCWInputDrain(uint32 session, uint32 consumer, uint32 output, uint32 bytes, uint32 maxEvents)
		{
			if (maxEvents == 0 || maxEvents > MCW_INPUT_MAX_DRAIN)
				return MCW_INPUT_INVALID_BUFFER;
			return Get().Drain(session, consumer, Output(output, bytes, MCW_INPUT_HEADER_SIZE + maxEvents * MCW_INPUT_EVENT_SIZE), maxEvents);
		}

		uint32 MCWInputCapture(uint32 session, uint32 consumer, uint32 sequence, uint32 flags, uint32 output, uint32 bytes)
		{
			return Get().Capture(session, consumer, sequence, flags, Output(output, bytes, MCW_INPUT_HEADER_SIZE));
		}

		class InputModule final : public COSModule
		{
		  public:
			std::string_view GetName() override
			{
				return "mcwiiu_input";
			}
			void RPLMapped() override
			{
				cafeExportRegister("mcwiiu_input", MCWInputPoll, LogType::InputAPI);
				cafeExportRegister("mcwiiu_input", MCWInputToggle, LogType::InputAPI);
				cafeExportRegister("mcwiiu_input", MCWInputQuery, LogType::InputAPI);
				cafeExportRegister("mcwiiu_input", MCWInputRegister, LogType::InputAPI);
				cafeExportRegister("mcwiiu_input", MCWInputUnregister, LogType::InputAPI);
				cafeExportRegister("mcwiiu_input", MCWInputDrain, LogType::InputAPI);
				cafeExportRegister("mcwiiu_input", MCWInputCapture, LogType::InputAPI);
			}
			void RPLUnmapped() override
			{
				Get().EndSession();
			}
		};
	} // namespace

	COSModule* GetModule()
	{
		static InputModule module;
		return &module;
	}
} // namespace guestinput
