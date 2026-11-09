#include "WorldImport.h"

#include <Windows.h>
#include <openssl/sha.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;
namespace
{
	using Bytes = WorldMetadata::Bytes;
	constexpr size_t s_bodyLimit = 512 * 1024 * 1024, s_extLimit = 1024 * 1024;
	void Require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
	std::string Hash(std::span<const uint8_t> bytes)
	{
		std::array<uint8_t, SHA256_DIGEST_LENGTH> hash{}; SHA256(bytes.data(), bytes.size(), hash.data());
		constexpr char hex[] = "0123456789abcdef"; std::string result;
		for (auto byte : hash) { result += hex[byte >> 4]; result += hex[byte & 15]; } return result;
	}
	uint32_t Read32(std::span<const uint8_t> bytes, size_t offset)
	{
		Require(offset <= bytes.size() && bytes.size() - offset >= 4, "invalid-body");
		return uint32_t(bytes[offset]) << 24 | uint32_t(bytes[offset + 1]) << 16 | uint32_t(bytes[offset + 2]) << 8 | bytes[offset + 3];
	}
	std::string Utf8(std::wstring_view value)
	{
		const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
		Require(size > 0, "unsafe-source-path"); std::string result(size, 0);
		WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr); return result;
	}
	std::wstring Lower(std::wstring_view value)
	{
		if (value.empty()) return {};
		const auto count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
		Require(count > 0, "unsafe-source-path"); std::wstring result(count, 0);
		LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr, 0); return result;
	}
	bool SaveId(std::wstring_view name) { return name.size() == 12 && std::all_of(name.begin(), name.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }); }
	bool AccountId(std::wstring_view name)
	{
		return name.size() == 8 && name[0] >= L'8' && name[0] <= L'f' && std::all_of(name.begin(), name.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); });
	}
	void CheckTitlePath(std::wstring path)
	{
		std::replace(path.begin(), path.end(), L'\\', L'/'); path = L"/" + Lower(path);
		if (const auto pos = path.find(L"/usr/save/"); pos != std::wstring::npos)
			Require(path.substr(pos + 10).starts_with(L"00050000/101dbe00/"), "unsupported-title");
		if (const auto pos = path.find(L"/00050000/"); pos != std::wstring::npos)
			Require(path.substr(pos + 10).starts_with(L"101dbe00/"), "unsupported-title");
	}

}

void WorldImport::ValidateBody(std::span<const uint8_t> body)
{
	Require(body.size() > 8 && body.size() <= s_bodyLimit && Read32(body, 0) == 0, "unsupported-body-format");
	const auto size = Read32(body, 4); Require(size >= 12 && size <= s_bodyLimit && uint64_t(size) <= uint64_t(body.size()) * 1000, "body-size-limit");
	Bytes raw(size); z_stream stream{}; Require(inflateInit(&stream) == Z_OK, "invalid-body");
	stream.next_in = const_cast<uint8_t*>(body.data() + 8); stream.avail_in = static_cast<uInt>(body.size() - 8);
	stream.next_out = raw.data(); stream.avail_out = size;
	const auto status = inflate(&stream, Z_FINISH);
	const bool valid = status == Z_STREAM_END && stream.total_out == size && stream.avail_in == 0; inflateEnd(&stream);
	Require(valid, "invalid-body");
	// 実際の JPN v560 の原本とゲーム保存後コピーで確認した、コンテナ形式10だけを受け付ける。
	Require(Read32(raw, 8) == 0x000a000a, "unsupported-body-format");
	const uint64_t table = Read32(raw, 0), count = Read32(raw, 4);
	Require(table >= 12 && count > 0 && count <= 4096 && table + count * 144 == raw.size(), "invalid-body");
	std::set<std::wstring> names; std::vector<std::pair<uint64_t, uint64_t>> ranges; bool level = false;
	for (size_t i = 0; i < count; ++i)
	{
		const auto entry = static_cast<size_t>(table) + i * 144; std::wstring name;
		for (size_t n = 0; n < 64; ++n)
		{
			const auto c = static_cast<wchar_t>(uint16_t(raw[entry + n * 2]) << 8 | raw[entry + n * 2 + 1]);
			if (!c) break; Require(n < 63, "invalid-body"); name += c;
		}
		Require(!name.empty() && names.insert(name).second, "invalid-body"); Utf8(name);
		Require(name.front() != L'/' && name.find(L'\\') == std::wstring::npos && name.find(L':') == std::wstring::npos && name.find(L"../") == std::wstring::npos && name != L"..", "invalid-body");
		const uint64_t length = Read32(raw, entry + 128), offset = Read32(raw, entry + 132);
		Require(offset >= 12 && offset <= table && length <= table - offset, "invalid-body");
		if (length) ranges.emplace_back(offset, offset + length);
		if (name == L"level.dat") { Require(length > 0, "invalid-body"); level = true; }
	}
	std::sort(ranges.begin(), ranges.end());
	for (size_t i = 1; i < ranges.size(); ++i) Require(ranges[i].first >= ranges[i - 1].second, "invalid-body");
	Require(level, "invalid-body");
}

struct WorldImport::Impl
{
	struct File
	{
		fs::path path;
		std::string id, display, hash;
		uint64_t size = 0;
		fs::file_time_type time;
	};
	Reader read;
	File body, metadata;
	Candidate candidate;
	std::string name, imageName, selectionError;
	Bytes rgba, generated;
	fs::file_time_type generatedTime;
	explicit Impl(Reader reader) : read(std::move(reader)) { Refresh(); }
	File Select(const fs::path& path, bool ext)
	{
		Require(path.is_absolute() && path.lexically_normal() == path, "unsafe-source-path"); CheckTitlePath(path.native() + L"/");
		auto id = path.filename().native();
		if (ext)
		{
			Require(Lower(path.extension().native()) == L".ext", "unsupported-metadata-file"); id = path.stem().native();
		}
		else Require(path.extension().empty(), "unsupported-import-format");
		Require(SaveId(id), "invalid-save-id"); Require(fs::is_regular_file(path), "unsupported-import-format");
		const auto bytes = read(path, ext ? s_extLimit : s_bodyLimit);
		if (ext) WorldMetadata::Parse(bytes); else ValidateBody(bytes);
		return {path, Utf8(id), Utf8(path.filename().native()), Hash(bytes), bytes.size(), fs::last_write_time(path)};
	}
	Bytes ReadFile(const File& file, size_t limit) const
	{
		const auto bytes = read(file.path, limit);
		Require(bytes.size() == file.size && Hash(bytes) == file.hash && fs::last_write_time(file.path) == file.time, "stale-import-source"); return bytes;
	}
	void ResetGenerated()
	{
		name.clear(); rgba.clear(); imageName.clear(); generated.clear();
	}
	void Refresh()
	{
		candidate = {}; generated.clear(); candidate.generated = metadata.path.empty();
		candidate.id = body.id; candidate.worldFile = body.display; candidate.metadataFile = metadata.display; candidate.imageFile = imageName; candidate.bodyHash = body.hash; candidate.size = body.size;
		if (!body.path.empty())
		{
			const auto owner = Lower(body.path.parent_path().filename().native()); if (AccountId(owner)) candidate.account = Utf8(owner);
		}
		candidate.name = name;
		try
		{
			Require(selectionError.empty(), selectionError.c_str());
			Require(!body.path.empty(), "world-file-required");
			Bytes ext;
			if (!metadata.path.empty())
			{
				Require(metadata.id == body.id, "metadata-id-mismatch"); ext = ReadFile(metadata, s_extLimit); const auto parsed = WorldMetadata::Parse(ext); candidate.name = parsed.name;
			}
			else
			{
				if (!rgba.empty()) candidate.icon = "data:image/png;base64," + WorldMetadata::Base64(WorldMetadata::Image(WorldMetadata::Create("Preview", rgba)));
				WorldMetadata::ValidateName(name); Require(!rgba.empty(), "import-icon-required");
				generated = WorldMetadata::Create(name, rgba); generatedTime = fs::file_time_type::clock::now(); ext = generated;
			}
			candidate.extHash = Hash(ext); candidate.size += ext.size(); candidate.icon = "data:image/png;base64," + WorldMetadata::Base64(WorldMetadata::Image(ext)); candidate.ready = true;
		}
		catch (const std::exception& error) { candidate.problem = Problem(error); }
	}
	static std::string Problem(const std::exception& error)
	{
		const std::set<std::string> reasons{"invalid-name","invalid-image","world-file-required","import-icon-required","metadata-id-mismatch","invalid-import-mode","stale-import-source","unsupported-metadata","unsupported-import-format","unsupported-metadata-file","invalid-save-id","unsafe-source-path","unsafe-path","unsupported-title","read-failed","invalid-file","invalid-body","unsupported-body-format","body-size-limit"};
		return reasons.contains(error.what()) ? error.what() : "invalid-file";
	}
};

WorldImport::WorldImport(Reader reader) : m_impl(std::make_unique<Impl>(std::move(reader))) {}
WorldImport::~WorldImport() = default;
const WorldImport::Candidate& WorldImport::Preview() const { return m_impl->candidate; }
void WorldImport::SelectBody(const fs::path& selected)
{
	auto& state = *m_impl; state.body = {}; state.ResetGenerated(); state.selectionError.clear();
	try { state.body = state.Select(selected, false); } catch (const std::exception& error) { state.selectionError = Impl::Problem(error); state.Refresh(); throw; }
	state.Refresh();
}
void WorldImport::SelectMetadata(const fs::path& selected)
{
	auto& state = *m_impl; state.metadata = {}; state.ResetGenerated(); state.selectionError.clear();
	try { state.metadata = state.Select(selected, true); } catch (const std::exception& error) { state.selectionError = Impl::Problem(error); state.Refresh(); throw; }
	state.Refresh();
}
void WorldImport::ClearMetadata()
{
	auto& state = *m_impl; state.metadata = {}; state.ResetGenerated(); state.selectionError.clear(); state.Refresh();
}
void WorldImport::SetName(std::string name)
{
	auto& state = *m_impl; Require(state.metadata.path.empty(), "invalid-import-mode"); state.name = std::move(name); state.selectionError.clear(); state.Refresh();
}
void WorldImport::ClearIcon()
{
	auto& state = *m_impl; Require(state.metadata.path.empty(), "invalid-import-mode"); state.rgba.clear(); state.imageName.clear(); state.Refresh();
}
void WorldImport::SetIcon(Bytes rgba, std::string displayName)
{
	auto& state = *m_impl; Require(state.metadata.path.empty(), "invalid-import-mode"); Require(rgba.size() == 64 * 64 * 4, "invalid-image");
	state.rgba = std::move(rgba); state.imageName = std::move(displayName); state.selectionError.clear(); state.Refresh();
}
WorldImport::Data WorldImport::ReadVerified() const
{
	const auto& state = *m_impl; Require(state.candidate.ready, "invalid-import-preview");
	Data data; data.body = state.ReadFile(state.body, s_bodyLimit); ValidateBody(data.body); data.bodyTime = state.body.time;
	if (state.candidate.generated)
	{
		data.ext = WorldMetadata::Create(state.name, state.rgba); data.extTime = state.generatedTime;
		Require(data.ext == state.generated, "stale-import-preview");
	}
	else { data.ext = state.ReadFile(state.metadata, s_extLimit); data.extTime = state.metadata.time; }
	WorldMetadata::Parse(data.ext); Require(Hash(data.ext) == state.candidate.extHash, "stale-import-preview"); return data;
}
