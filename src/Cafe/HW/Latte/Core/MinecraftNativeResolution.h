#pragma once

#include <cstdint>

namespace MinecraftNativeResolution
{
	// タイトル開始前と GPU join 後だけ、セッションの設定を変更する。
	bool Begin(uint64_t titleId, uint16_t version, uint32_t width, uint32_t height);
	void End();
	bool IsActive();
	bool GetSize(uint32_t width, uint32_t height, uint32_t format, uint32_t& effectiveWidth, uint32_t& effectiveHeight);
} // namespace MinecraftNativeResolution
