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
	struct Language { unsigned config_version = 1; std::string language = "auto"; };

	std::filesystem::path LanguagePath(const std::filesystem::path& path)
	{
		auto language = path;
		language += L".language.json";
		return language;
	}

	bool ReadSettings(const std::filesystem::path& path, std::string& json, std::string& notice)
	{
		notice = "default";
		const auto attributes = GetFileAttributesW(path.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) return false;
		std::error_code error;
		if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
			!std::filesystem::is_regular_file(path, error) || error || std::filesystem::file_size(path, error) > 1024 || error)
		{
			notice = "access-error";
			return false;
		}
		std::ifstream stream(path, std::ios::binary);
		std::array<char, 1025> bytes{};
		stream.read(bytes.data(), bytes.size());
		if (stream.bad()) { notice = "access-error"; return false; }
		json.assign(bytes.data(), static_cast<size_t>(stream.gcount()));
		notice = "loaded";
		return true;
	}

	bool WriteTemporary(const std::filesystem::path& path, std::string_view json)
	{
		const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return false;
		DWORD written = 0;
		const bool flushed = WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written, nullptr) &&
			written == json.size() && FlushFileBuffers(file);
		const bool closed = CloseHandle(file) != FALSE;
		if (!flushed || !closed) DeleteFileW(path.c_str());
		return flushed && closed;
	}
}

using namespace LauncherPreferencesDetail;

bool LauncherPreferences::Parse(std::string_view json, LauncherPreferences& preferences)
{
	if (json.empty() || json.size() > 1024 || json.find('\0') != std::string_view::npos || glz::validate_json(json))
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
	std::string json;
	if (ReadSettings(path, json, notice))
	{
		Schema schema;
		const auto error = glz::read<glz::opts{.error_on_unknown_keys = false}>(schema, json);
		if (!Parse(json, preferences)) notice = !error && schema.config_version > 3 ? "future-version" : "invalid-json";
		else notice = schema.config_version == 2 ? "loaded" : "legacy-loaded";
	}
	if (notice == "loaded" || notice == "legacy-loaded" || notice == "default")
	{
		std::string languageNotice;
		if (ReadSettings(LanguagePath(path), json, languageNotice))
		{
			Schema schema;
			Language language;
			if (!glz::read<glz::opts{.error_on_unknown_keys = false}>(schema, json) && schema.config_version > 1)
				languageNotice = "future-version";
			else if (json.empty() || json.size() > 1024 || json.find('\0') != std::string::npos || glz::validate_json(json) ||
				glz::read<glz::opts{.error_on_missing_keys = true}>(language, json) || language.config_version != 1)
				languageNotice = "invalid-json";
			else
			{
				constexpr std::array languages{"auto", "en", "ja", "ko", "zh-CN", "zh-TW", "es", "fr", "de"};
				if (std::find(languages.begin(), languages.end(), language.language) == languages.end()) languageNotice = "invalid-json";
				else preferences.language = language.language;
			}
		}
		if (languageNotice != "loaded" && languageNotice != "default") notice = languageNotice;
	}
	if (notice != "loaded" && notice != "legacy-loaded" && notice != "default") preferences.check_on_startup = false;
	return preferences;
}

std::string LauncherPreferences::Json() const
{
	return glz::write_json(*this).value();
}

std::string LauncherPreferences::StorageJson() const
{
	// 共有設定は v0.4.0 が未知キーなしで読める v2 に固定する。
	return glz::write_json(Version2{2, check_on_startup}).value();
}

bool LauncherPreferences::Save(const std::filesystem::path& path) const
{
	LauncherPreferences checked;
	if (!Parse(Json(), checked) || checked != *this) return false;
	std::string notice;
	Load(path, notice);
	if (notice != "default" && notice != "loaded" && notice != "legacy-loaded") return false;

	auto temporary = path;
	temporary += L".tmp";
	const auto languagePath = LanguagePath(path);
	auto languageTemporary = languagePath;
	languageTemporary += L".tmp";
	// 両方を確保・flush してから各ファイルを置換する。既存の一時ファイルは保持する。
	if (!WriteTemporary(temporary, StorageJson())) return false;
	if (!WriteTemporary(languageTemporary, glz::write_json(Language{1, language}).value()))
	{
		DeleteFileW(temporary.c_str());
		return false;
	}
	if (!MoveFileExW(languageTemporary.c_str(), languagePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ||
		!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		DeleteFileW(temporary.c_str());
		DeleteFileW(languageTemporary.c_str());
		return false;
	}
	return Load(path, notice) == *this && notice == "loaded";
}
