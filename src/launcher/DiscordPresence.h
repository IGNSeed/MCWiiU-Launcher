#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

namespace Launcher
{
	// 呼出元は状態を渡すだけとし、SDK は専用 worker からのみ操作する。
	class DiscordPresence final
	{
	public:
		explicit DiscordPresence(uint64_t processStart);
		~DiscordPresence();
		DiscordPresence(const DiscordPresence&) = delete;
		DiscordPresence& operator=(const DiscordPresence&) = delete;
		// Primary process の作成時刻を Unix epoch milliseconds で一度だけ取得する。
		static uint64_t ProcessStartTime();
		void Start() noexcept;
		bool SetPage(std::string_view page);
		void SetPlaying(bool playing);
		void Stop() noexcept;

	private:
		struct Impl;
		std::shared_ptr<Impl> m_impl;
	};
} // namespace Launcher
