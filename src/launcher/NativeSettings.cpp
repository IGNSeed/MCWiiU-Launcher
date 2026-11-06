#include "NativeSettings.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

bool NativeSettings::Parse(std::string_view json, NativeSettings& settings)
{
	if (json.size() > 1024 || json.find('\0') != std::string_view::npos)
		return false;
	rapidjson::Document doc;
	doc.Parse(json.data(), json.size());
	constexpr std::array names{"config_version", "native_vsync_frequency", "native_internal_width", "native_internal_height"};
	if (doc.HasParseError() || !doc.IsObject() || doc.MemberCount() != names.size())
		return false;
	for (const auto name : names)
		if (!doc.HasMember(name) || !doc[name].IsUint()) return false;
	if (doc["config_version"].GetUint() != 1)
		return false;
	const auto fps = doc["native_vsync_frequency"].GetUint();
	if (fps != 0 && (fps < 30 || fps > 1000))
		return false;
	const auto width = doc["native_internal_width"].GetUint(), height = doc["native_internal_height"].GetUint();
	constexpr std::array<std::pair<uint32_t, uint32_t>, 9> sizes{{{0, 0}, {640, 360}, {960, 540}, {1280, 720},
		{1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}}};
	if (std::find(sizes.begin(), sizes.end(), std::pair{width, height}) == sizes.end())
		return false;
	settings = {fps, width, height};
	return true;
}

NativeSettings NativeSettings::Load(const std::filesystem::path& path, std::string& notice)
{
	NativeSettings settings;
	notice = "default";
	std::error_code error;
	if (!std::filesystem::exists(path, error) && !error)
		return settings;
	if (error || !std::filesystem::is_regular_file(path, error) || error || std::filesystem::file_size(path, error) > 1024 || error)
	{
		notice = "load-failed";
		return settings;
	}
	std::ifstream stream(path, std::ios::binary);
	std::array<char, 1025> json{};
	stream.read(json.data(), json.size());
	if (!stream.bad() && Parse(std::string_view(json.data(), static_cast<size_t>(stream.gcount())), settings)) notice = "loaded";
	else notice = "load-failed";
	return settings;
}

std::string NativeSettings::Json() const
{
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	writer.StartObject();
	writer.Key("config_version"); writer.Uint(1);
	writer.Key("native_vsync_frequency"); writer.Uint(vsyncFrequency);
	writer.Key("native_internal_width"); writer.Uint(width);
	writer.Key("native_internal_height"); writer.Uint(height);
	writer.EndObject();
	return buffer.GetString();
}

bool NativeSettings::Save(const std::filesystem::path& path) const
{
	NativeSettings checked;
	const auto json = Json();
	if (!Parse(json, checked)) return false;
	auto temporary = path;
	temporary += L".tmp";
	// CREATE_NEW により、既存の一時ファイルやリンクを上書きしない。
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
