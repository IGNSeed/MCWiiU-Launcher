#include "MinecraftNativeResolution.h"

#include <atomic>

namespace MinecraftNativeResolution
{
	static std::atomic<uint64_t> s_size{0};

	bool Begin(uint64_t titleId, uint16_t version, uint32_t width, uint32_t height)
	{
		End();
		if (titleId != 0x00050000101dbe00ULL || version != 560)
			return false;
		if (!width && !height)
			return true;
		const bool supported = (width == 640 && height == 360) || (width == 960 && height == 540) ||
			(width == 1280 && height == 720) || (width == 1600 && height == 900) ||
			(width == 1920 && height == 1080) || (width == 2560 && height == 1440) ||
			(width == 3200 && height == 1800) || (width == 3840 && height == 2160);
		if (!supported)
			return false;
		s_size.store((uint64_t(width) << 32) | height);
		return true;
	}

	void End() { s_size.store(0); }
	bool IsActive() { return s_size.load() != 0; }

	bool GetSize(uint32_t width, uint32_t height, uint32_t format, uint32_t& effectiveWidth, uint32_t& effectiveHeight)
	{
		const auto size = s_size.load();
		if (!size)
			return false;
		const bool tv = width == 1280 && height == 720 && (format == 0x1a || format == 0x11 || format == 0x820);
		const bool pad = width == 864 && height == 480 && (format == 0x1a || format == 0x11);
		const bool pad854 = width == 854 && height == 480 && format == 0x1a;
		if (!tv && !pad && !pad854)
			return false;
		// ExpressionParser<double>::Evaluate<int> と同じ、倍率計算後の切捨て。
		effectiveWidth = static_cast<uint32_t>((double(size >> 32) / 1280.0) * width);
		effectiveHeight = static_cast<uint32_t>((double(uint32_t(size)) / 720.0) * height);
		return true;
	}
} // namespace MinecraftNativeResolution
