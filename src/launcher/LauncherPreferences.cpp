#include "LauncherPreferences.h"

#include <Windows.h>
#include <array>
#include <algorithm>
#include <fstream>
#include <glaze/glaze.hpp>

namespace LauncherPreferencesDetail
{
	struct Schema { unsigned config_version = 0; };
	struct Version2 { unsigned config_version = 2; bool check_on_startup = true; };
	struct Version3 { unsigned config_version = 3; bool check_on_startup = true; glz::raw_json language; };
	struct Legacy { unsigned config_version = 1; bool automatic_updates = false, check_on_startup = false, preview_updates = false; };
}

using namespace LauncherPreferencesDetail;

bool LauncherPreferences::Parse(std::string_view json, LauncherPreferences& preferences)
{
	if (json.empty() || json.size() > 1024 || json.find('\0') != std::string_view::npos)
		return false;
	Schema schema;
	if (glz::read<glz::opts{.error_on_unknown_keys = false}>(schema, json)) return false;
	LauncherPreferences checked;
	if (schema.config_version == 1)
	{
		Legacy legacy;
		if (glz::read<glz::opts{.error_on_missing_keys = true}>(legacy, json)) return false;
		checked.check_on_startup = legacy.check_on_startup;
	}
	else if (schema.config_version == 2)
	{
		Version2 previous;
		if (glz::read<glz::opts{.error_on_missing_keys = true}>(previous, json)) return false;
		checked.check_on_startup = previous.check_on_startup;
	}
	else if (schema.config_version == 3)
	{
		Version3 current;
		if (glz::read<glz::opts{.error_on_missing_keys = true}>(current, json)) return false;
		checked.check_on_startup = current.check_on_startup;
		if (glz::read_json(checked.language, current.language.str)) checked.language = "en";
	}
	else return false;
	constexpr std::array languages{"auto", "en", "ja", "ko", "zh-CN", "zh-TW", "es", "fr", "de"};
	if (std::find(languages.begin(), languages.end(), checked.language) == languages.end()) checked.language = "en";
	preferences = checked;
	return true;
}

LauncherPreferences LauncherPreferences::Load(const std::filesystem::path& path, std::string& notice)
{
	LauncherPreferences preferences;
	notice = "default";
	const auto attributes = GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND)
		return preferences;
	std::error_code error;
	if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
		!std::filesystem::is_regular_file(path, error) || error || std::filesystem::file_size(path, error) > 1024 || error)
	{
		notice = "access-error";
		preferences.check_on_startup = false;
		return preferences;
	}
	std::ifstream stream(path, std::ios::binary);
	std::array<char, 1025> json{};
	stream.read(json.data(), json.size());
	if (!stream.bad() && Parse(std::string_view(json.data(), static_cast<size_t>(stream.gcount())), preferences))
	{
		Schema schema;
		glz::read<glz::opts{.error_on_unknown_keys = false}>(schema, std::string_view(json.data(), static_cast<size_t>(stream.gcount())));
		notice = schema.config_version < 3 ? "legacy-loaded" : "loaded";
	}
	else
	{
		Schema schema;
		const auto bytes = std::string_view(json.data(), static_cast<size_t>(stream.gcount()));
		notice = !glz::read<glz::opts{.error_on_unknown_keys = false}>(schema, bytes) && schema.config_version > 3 ? "future-version" : "invalid-json";
		preferences.check_on_startup = false;
	}
	return preferences;
}

std::string LauncherPreferences::Json() const
{
	return glz::write_json(*this).value();
}

bool LauncherPreferences::Save(const std::filesystem::path& path) const
{
	LauncherPreferences checked;
	const auto json = Json();
	if (!Parse(json, checked)) return false;
	const auto attributes = GetFileAttributesW(path.c_str());
	if (attributes != INVALID_FILE_ATTRIBUTES)
	{
		std::string notice; Load(path, notice);
		if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) || (notice != "loaded" && notice != "legacy-loaded")) return false;
	}
	auto temporary = path;
	temporary += L".tmp";
	// Native 設定と同じく、既存ファイルを保持したまま flush 後に置換する。
	const auto file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	DWORD written = 0;
	const bool flushed = WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written, nullptr) &&
		written == json.size() && FlushFileBuffers(file);
	const bool closed = CloseHandle(file) != FALSE;
	if (!flushed || !closed || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		DeleteFileW(temporary.c_str());
		return false;
	}
	std::string notice;
	return Load(path, notice) == *this && notice == "loaded";
}
