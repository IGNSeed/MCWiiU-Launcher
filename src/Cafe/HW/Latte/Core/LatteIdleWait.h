#pragma once

#include <cstdint>

namespace LatteIdleWait
{
	// タイトル開始前と GPU join 後だけ変更する。既定は無効。
	void SetEnabled(bool enabled);
	bool IsEnabled();
	std::uint64_t GetSequence();
	void Notify();
	void Wait(std::uint64_t sequence, std::uint64_t timeout100ns);
} // namespace LatteIdleWait
