#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

struct NativeSettings
{
	uint32_t vsyncFrequency = 1000;
	uint32_t width = 1920, height = 1080;
	bool operator==(const NativeSettings&) const = default;

	static bool Parse(std::string_view json, NativeSettings& settings);
	static NativeSettings Load(const std::filesystem::path& path, std::string& notice);
	std::string Json() const;
	bool Save(const std::filesystem::path& path) const;
};
