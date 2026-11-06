#include "LauncherPreferences.h"

#include <Windows.h>
#include <array>
#include <fstream>
#include <glaze/glaze.hpp>

bool LauncherPreferences::Parse(std::string_view json, LauncherPreferences& preferences)
{
	if (json.empty() || json.size() > 1024 || json.find('\0') != std::string_view::npos)
		return false;
	LauncherPreferences checked;
	if (glz::read<glz::opts{.error_on_missing_keys = true}>(checked, json) || checked.config_version != 1)
		return false;
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
		notice = "load-failed";
		return preferences;
	}
	std::ifstream stream(path, std::ios::binary);
	std::array<char, 1025> json{};
	stream.read(json.data(), json.size());
	if (!stream.bad() && Parse(std::string_view(json.data(), static_cast<size_t>(stream.gcount())), preferences)) notice = "loaded";
	else notice = "load-failed";
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
	if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
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
