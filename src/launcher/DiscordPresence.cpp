#include "DiscordPresence.h"
#include "DiscordPresenceState.h"

#define DISCORDPP_IMPLEMENTATION
#include <discordpp.h>

#include <Windows.h>
#include <delayimp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

namespace
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// 遅延 import の失敗を SDK 呼出前に処理する。SEH 内に C++ 所有物を置かない。
	bool ResolveSdkImports()
	{
		__try
		{
			return SUCCEEDED(__HrLoadAllImportsForDll("discord_partner_sdk.dll"));
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool LoadSdk()
	{
		wchar_t image[32768]{};
		const auto length = GetModuleFileNameW(nullptr, image, static_cast<DWORD>(std::size(image)));
		if (!length || length >= std::size(image)) return false;
		const auto library = std::filesystem::path(image).parent_path() / L"discord_partner_sdk.dll";
		const auto matchesPath = [&](HMODULE module) {
			wchar_t loaded[32768]{};
			const auto loadedLength = GetModuleFileNameW(module, loaded, static_cast<DWORD>(std::size(loaded)));
			return loadedLength && loadedLength < std::size(loaded) &&
				CompareStringOrdinal(library.c_str(), -1, loaded, -1, TRUE) == CSTR_EQUAL;
		};
		// 遅延 import が別 directory の既存同名 module を再利用することも防ぐ。
		if (const auto existing = GetModuleHandleW(L"discord_partner_sdk.dll"); existing && !matchesPath(existing)) return false;
		const auto module = LoadLibraryExW(library.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (!module) return false;
		const bool valid = matchesPath(module) && ResolveSdkImports();
		// 遅延 import の IAT が参照する module は process 寿命で保持する。
		if (!valid) FreeLibrary(module);
		return valid;
	}

	unsigned DesktopEndpoints()
	{
		unsigned endpoints = 0;
		for (unsigned index = 0; index != 10; ++index)
		{
			const auto pipe = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(index);
			if (WaitNamedPipeW(pipe.c_str(), NMPWAIT_NOWAIT) || GetLastError() == ERROR_SEM_TIMEOUT || GetLastError() == ERROR_PIPE_BUSY)
				endpoints |= 1u << index;
		}
		return endpoints;
	}
} // namespace

namespace Launcher
{
	struct DiscordPresence::Impl
	{
		std::mutex m_mutex;
		std::condition_variable m_wake, m_finished;
		DiscordPresenceState m_state;
		bool m_stop = false, m_done = false, m_changed = false;
		std::thread m_worker;

		explicit Impl(uint64_t processStart) : m_state(processStart) {}

		void Run() noexcept
		{
			try
			{
				RunSdk();
			} catch (...)
			{
				OutputDebugStringA("MCWiiU Launcher: Discord presence unavailable; Launcher remains usable\n");
			}
			{
				std::lock_guard lock(m_mutex);
				m_done = true;
			}
			m_finished.notify_all();
		}

		void RunSdk()
		{
			if (!LoadSdk())
			{
				OutputDebugStringA("MCWiiU Launcher: bundled Discord SDK missing or incompatible\n");
				return;
			}
			// RPC のみを使用し、Connect / OAuth / ユーザー情報 API は呼ばない。
			auto client = std::make_unique<discordpp::Client>();
			client->SetApplicationId(DiscordPresenceState::ApplicationId);
			OutputDebugStringA(("MCWiiU Discord: initialized SDK " + std::to_string(discordpp::Client::GetVersionMajor()) + "." +
				std::to_string(discordpp::Client::GetVersionMinor()) + "." + std::to_string(discordpp::Client::GetVersionPatch()) +
				" application=" + std::to_string(client->GetApplicationId()) + "\n").c_str());
			struct Attempt
			{
				std::atomic_bool done{false}, success{false};
				DiscordPresenceState::Activity activity;
				Clock::time_point sent;
				unsigned connection;
			};
			std::shared_ptr<Attempt> pending;
			std::optional<DiscordPresenceState::Activity> delivered;
			auto nextSend = Clock::now(), nextProbe = nextSend;
			auto delay = 1s;
			unsigned endpoints = 0;
			unsigned connection = 0;
			while (true)
			{
				DiscordPresenceState::Activity desired;
				{
					std::unique_lock lock(m_mutex);
					if (m_stop) break;
					desired = m_state.Current();
					m_changed = false;
				}
				discordpp::RunCallbacks();
				const auto now = Clock::now();
				if (now >= nextProbe)
				{
					const auto current = DesktopEndpoints();
					if (current != endpoints)
					{
						++connection;
						delivered.reset();
					}
					if (current && current != endpoints)
					{
						delivered.reset();
						nextSend = now;
						delay = 1s;
					}
					endpoints = current;
					nextProbe = now + 1s;
				}
				if (pending && pending->done.load())
				{
					if (pending->success.load() && pending->connection == connection)
					{
						delivered = pending->activity;
						delay = 1s;
					}
					else
					{
						nextSend = std::max(nextSend, now + delay);
						delay = std::min(delay * 2, 60s);
					}
					pending.reset();
				}
				if (pending && now - pending->sent >= 15s)
				{
					// 応答を失った RPC の callback は weak 参照で無効化する。
					pending.reset();
					client.reset();
					client = std::make_unique<discordpp::Client>();
					client->SetApplicationId(DiscordPresenceState::ApplicationId);
					delivered.reset();
					nextSend = now + delay;
					delay = std::min(delay * 2, 60s);
				}
				if (!pending && now >= nextSend && (!delivered || *delivered != desired))
				{
					discordpp::Activity activity;
					activity.SetType(discordpp::ActivityTypes::Playing);
					activity.SetDetails(std::string(desired.details));
					activity.SetState(std::string(desired.state));
					discordpp::ActivityAssets assets;
					assets.SetLargeImage(std::string(DiscordPresenceState::LargeImage));
					assets.SetLargeText(std::string(DiscordPresenceState::LargeText));
					activity.SetAssets(std::move(assets));
					discordpp::ActivityTimestamps timestamps;
					timestamps.SetStart(desired.start);
					activity.SetTimestamps(std::move(timestamps));
					pending = std::make_shared<Attempt>();
					pending->activity = desired;
					pending->sent = now;
					pending->connection = connection;
					OutputDebugStringA(("MCWiiU Discord: update details=" + std::string(desired.details) + " state=" +
						std::string(desired.state) + " start_ms=" + std::to_string(desired.start) + " asset=" +
						std::string(DiscordPresenceState::LargeImage) + "\n").c_str());
					client->UpdateRichPresence(std::move(activity), [weak = std::weak_ptr<Attempt>(pending)](discordpp::ClientResult result) {
						if (const auto attempt = weak.lock())
						{
							attempt->success = result.Successful();
							attempt->done = true;
							OutputDebugStringA(("MCWiiU Discord: result=" + std::to_string(static_cast<int>(result.Type())) +
								" callback_thread=" + std::to_string(GetCurrentThreadId()) + "\n").c_str());
						}
					});
					nextSend = now + 2s;
				}
				std::unique_lock lock(m_mutex);
				m_wake.wait_for(lock, 50ms, [&] { return m_stop || m_changed; });
			}
			pending.reset();
			client->ClearRichPresence();
			const auto deadline = Clock::now() + 250ms;
			while (Clock::now() < deadline)
			{
				discordpp::RunCallbacks();
				std::this_thread::sleep_for(10ms);
			}
			client.reset();
			OutputDebugStringA("MCWiiU Discord: stopped\n");
		}
	};

	uint64_t DiscordPresence::ProcessStartTime()
	{
		static const uint64_t start = [] {
			FILETIME creation{}, exit{}, kernel{}, user{};
			if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
			{
				const auto ticks = (uint64_t{creation.dwHighDateTime} << 32) | creation.dwLowDateTime;
				return uint64_t{(ticks - 116444736000000000ULL) / 10000ULL};
			}
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
		}();
		return start;
	}

	DiscordPresence::DiscordPresence(uint64_t processStart) : m_impl(std::make_shared<Impl>(processStart)) {}
	DiscordPresence::~DiscordPresence() { Stop(); }

	void DiscordPresence::Start() noexcept
	{
		try
		{
			std::lock_guard lock(m_impl->m_mutex);
			if (m_impl->m_stop || m_impl->m_done || m_impl->m_worker.joinable()) return;
			m_impl->m_worker = std::thread([state = m_impl] { state->Run(); });
		} catch (...)
		{
			OutputDebugStringA("MCWiiU Launcher: could not start Discord worker\n");
		}
	}

	bool DiscordPresence::SetPage(std::string_view page)
	{
		std::lock_guard lock(m_impl->m_mutex);
		const auto previous = m_impl->m_state.Current();
		if (m_impl->m_stop || !m_impl->m_state.SetPage(page)) return false;
		if (previous != m_impl->m_state.Current())
		{
			m_impl->m_changed = true;
			m_impl->m_wake.notify_one();
		}
		return true;
	}

	void DiscordPresence::SetPlaying(bool playing)
	{
		std::lock_guard lock(m_impl->m_mutex);
		if (m_impl->m_stop) return;
		const auto previous = m_impl->m_state.Current();
		m_impl->m_state.SetPlaying(playing);
		if (previous != m_impl->m_state.Current())
		{
			m_impl->m_changed = true;
			m_impl->m_wake.notify_one();
		}
	}

	void DiscordPresence::Stop() noexcept
	{
		std::unique_lock lock(m_impl->m_mutex);
		m_impl->m_stop = true;
		m_impl->m_wake.notify_all();
		if (!m_impl->m_worker.joinable()) return;
		const bool done = m_impl->m_finished.wait_for(lock, std::chrono::milliseconds(1500), [&] { return m_impl->m_done; });
		lock.unlock();
		if (done) m_impl->m_worker.join();
		else
		{
			// SDK 内部の停止が遅くても UI の終了を有限にする。worker の所有物は残す。
			m_impl->m_worker.detach();
			OutputDebugStringA("MCWiiU Launcher: Discord shutdown exceeded deadline\n");
		}
	}
} // namespace Launcher
