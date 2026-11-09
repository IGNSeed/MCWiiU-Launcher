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
	fs::path bodyPath, extPath;
	Reader read;
	Candidate candidate;
	fs::file_time_type bodyTime, extTime;

	Impl(fs::path selected, Reader reader) : read(std::move(reader))
	{
		Require(selected.is_absolute() && selected.lexically_normal() == selected, "unsafe-source-path");
		CheckTitlePath(selected.native() + L"/");
		auto base = selected;
		const auto extension = Lower(base.extension().native());
		Require(extension.empty() || extension == L".ext", "unsupported-import-format");
		if (!extension.empty()) base.replace_extension();
		Require(SaveId(base.filename().native()), "invalid-save-id");
		bodyPath = base; extPath = fs::path(base.native() + L".ext");
		Require(fs::exists(bodyPath) && fs::exists(extPath), "missing-pair");
		Require(fs::is_regular_file(bodyPath) && fs::is_regular_file(extPath), "unsupported-import-format");
		const auto body = read(bodyPath, s_bodyLimit), ext = read(extPath, s_extLimit);
		ValidateBody(body); const auto metadata = WorldMetadata::Parse(ext);
		bodyTime = fs::last_write_time(bodyPath); extTime = fs::last_write_time(extPath);
		candidate.id = Utf8(base.filename().native());
		const auto owner = Lower(base.parent_path().filename().native());
		if (AccountId(owner)) candidate.account = Utf8(owner);
		candidate.name = metadata.name; candidate.size = body.size() + ext.size();
		candidate.bodyHash = Hash(body); candidate.extHash = Hash(ext);
		candidate.icon = "data:image/png;base64," + WorldMetadata::Base64(WorldMetadata::Image(ext));

	}
};

WorldImport::WorldImport(fs::path selected, Reader reader) : m_impl(std::make_unique<Impl>(std::move(selected), std::move(reader))) {}
WorldImport::~WorldImport() = default;
const WorldImport::Candidate& WorldImport::Preview() const { return m_impl->candidate; }
WorldImport::Data WorldImport::ReadVerified() const
{
	const auto& state = *m_impl; const auto& candidate = state.candidate;
	Data data{state.read(state.bodyPath, s_bodyLimit), state.read(state.extPath, s_extLimit), state.bodyTime, state.extTime};
	Require(Hash(data.body) == candidate.bodyHash && Hash(data.ext) == candidate.extHash &&
		fs::last_write_time(state.bodyPath) == state.bodyTime && fs::last_write_time(state.extPath) == state.extTime, "stale-import-source");
	ValidateBody(data.body); WorldMetadata::Parse(data.ext); return data;
}
