#pragma once

#include <filesystem>
#include <string>
#include <string_view>

struct LauncherPreferences
{
	// UI 通信用の版。共有ファイルは StorageJson() で旧版互換の v2 を使う。
	unsigned config_version = 3;
	bool check_on_startup = true;
	std::string language = "auto";
	bool operator==(const LauncherPreferences&) const = default;
	static bool Parse(std::string_view json, LauncherPreferences& preferences);
	static LauncherPreferences Load(const std::filesystem::path& path, std::string& notice);
	std::string Json() const;
	std::string StorageJson() const;
	bool Save(const std::filesystem::path& path) const;
};
