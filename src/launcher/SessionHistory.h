#pragma once

#include "LauncherPaths.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

// Core の開始・停止イベントだけを受け取り、描画とは独立して履歴を保存する。
class SessionHistory final
{
public:
	struct ClockSample
	{
		uint64_t awakeUs = 0, utcUs = 0;
		DYNAMIC_TIME_ZONE_INFORMATION zone{};
	};
	using Clock = std::function<ClockSample()>;
	static ClockSample ReadClock();
	explicit SessionHistory(const LauncherPaths& paths, Clock clock = ReadClock);
	~SessionHistory();
	SessionHistory(const SessionHistory&) = delete;
	SessionHistory& operator=(const SessionHistory&) = delete;
	void Begin(uint64_t sessionId);
	void End(uint64_t sessionId);
	void ClockChanged();
	bool IsDurable() const;
	std::string Query(std::string_view request);
	std::string NoticeJson() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
