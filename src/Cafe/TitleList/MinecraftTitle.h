#pragma once

#include <array>
#include <cstdint>

namespace MinecraftTitle
{
	inline constexpr std::array<uint32_t, 3> TitleLow{0x101dbe00, 0x101d9d00, 0x101d7500};
	constexpr bool IsBase(uint64_t id)
	{
		for (const auto low : TitleLow)
			if (id == (0x0005000000000000ULL | low)) return true;
		return false;
	}
	constexpr uint64_t UpdateId(uint64_t base) { return IsBase(base) ? base | 0x0000000e00000000ULL : 0; }
	constexpr uint64_t DlcId(uint64_t base) { return IsBase(base) ? base | 0x0000000c00000000ULL : 0; }
	constexpr const char* RegionName(uint64_t base)
	{
		return base == 0x00050000101dbe00ULL ? "JPN" : base == 0x00050000101d9d00ULL ? "USA" : base == 0x00050000101d7500ULL ? "EUR" : "None";
	}
} // namespace MinecraftTitle
