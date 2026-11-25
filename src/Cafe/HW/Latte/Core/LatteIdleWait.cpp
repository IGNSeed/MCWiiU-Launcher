#include "Cafe/HW/Latte/Core/LatteIdleWait.h"

#include <atomic>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace LatteIdleWait
{
	namespace
	{
		std::atomic_bool s_enabled{false};
		std::atomic_bool s_waiting{false};
		std::atomic<std::uint64_t> s_sequence{0};

#if defined(_WIN32)
		struct WaitHandles
		{
			HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

			~WaitHandles()
			{
				if (event) CloseHandle(event);
				if (timer) CloseHandle(timer);
			}
		};

		WaitHandles& GetHandles()
		{
			static WaitHandles handles;
			return handles;
		}
#endif
	} // namespace

	void SetEnabled(bool enabled)
	{
#if defined(_WIN32)
		if (enabled)
		{
			auto& handles = GetHandles();
			enabled = handles.event && handles.timer;
			if (enabled) ResetEvent(handles.event);
		}
		s_enabled.store(enabled, std::memory_order::release);
#endif
	}

	bool IsEnabled()
	{
		return s_enabled.load(std::memory_order::acquire);
	}

	std::uint64_t GetSequence()
	{
		return s_sequence.load();
	}

	void Notify()
	{
		if (!IsEnabled())
			return;
		// 公開済みの仕事を世代で記録し、待機登録と同じ全順序で照合する。
		s_sequence.fetch_add(1);
#if defined(_WIN32)
		if (s_waiting.load()) SetEvent(GetHandles().event);
#endif
	}

	void Wait(std::uint64_t sequence, std::uint64_t timeout100ns)
	{
#if defined(_WIN32)
		if (!IsEnabled() || !timeout100ns)
			return;
		auto& handles = GetHandles();
		LARGE_INTEGER due;
		due.QuadPart = -static_cast<LONGLONG>(timeout100ns);
		if (!SetWaitableTimer(handles.timer, &due, 0, nullptr, nullptr, FALSE))
			return;
		s_waiting.store(true);
		// 登録前の通知は世代差、登録後の通知は auto-reset event が保持する。
		// 古い通知・spurious wakeup は呼出側で仕事と期限を再確認する。
		if (GetSequence() == sequence)
		{
			HANDLE objects[]{handles.event, handles.timer};
			WaitForMultipleObjects(2, objects, FALSE, INFINITE);
		}
		s_waiting.store(false);
#endif
	}
} // namespace LatteIdleWait
