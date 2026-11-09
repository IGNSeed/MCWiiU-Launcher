#include "WorldImport.h"

#include <Windows.h>
#include <openssl/sha.h>
#include <zip.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;
namespace
{
	using Bytes = WorldMetadata::Bytes;
	constexpr size_t s_bodyLimit = 512 * 1024 * 1024, s_extLimit = 1024 * 1024, s_totalLimit = 1024 * 1024 * 1024;
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
	std::wstring Wide(std::string_view value)
	{
		Require(!value.empty() && value.size() <= 4096 && value.find('\0') == std::string_view::npos, "unsafe-zip-path");
		const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
		Require(size > 0, "unsafe-zip-path"); std::wstring result(size, 0);
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size); return result;
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
	std::wstring ZipPath(std::string_view name)
	{
		auto wide = Wide(name); Require(wide.size() <= 512 && wide.front() != L'/' && wide.find(L'\\') == std::wstring::npos, "unsafe-zip-path");
		if (wide.back() == L'/') wide.pop_back(); Require(!wide.empty(), "unsafe-zip-path");
		for (size_t start = 0; start < wide.size();)
		{
			const auto end = wide.find(L'/', start); const auto part = wide.substr(start, end == std::wstring::npos ? end : end - start);
			Require(!part.empty() && part.size() <= 255 && part != L"." && part != L".." && part.back() != L'.' && part.back() != L' ', "unsafe-zip-path");
			Require(std::none_of(part.begin(), part.end(), [](wchar_t c) { return c < 32 || c == 127 || std::wstring_view(L"<>:\"|?*").find(c) != std::wstring_view::npos; }), "unsafe-zip-path");
			const auto dot = part.find(L'.'); const auto stem = Lower(std::wstring_view(part).substr(0, dot));
			Require(stem != L"con" && stem != L"prn" && stem != L"aux" && stem != L"nul" && stem != L"conin$" && stem != L"conout$" &&
				!((stem.starts_with(L"com") || stem.starts_with(L"lpt")) && stem.size() == 4 && (stem[3] >= L'0' && stem[3] <= L'9' || stem[3] == 0xb9 || stem[3] == 0xb2 || stem[3] == 0xb3)), "unsafe-zip-path");
			if (end == std::wstring::npos) break; start = end + 1;
		}
		return Lower(wide);
	}
	struct Zip
	{
		Bytes bytes; zip_t* archive = nullptr;
		explicit Zip(Bytes input) : bytes(std::move(input))
		{
			zip_error_t error; zip_error_init(&error);
			auto* source = zip_source_buffer_create(bytes.data(), bytes.size(), 0, &error);
			if (source) { archive = zip_open_from_source(source, ZIP_RDONLY | ZIP_CHECKCONS, &error); if (!archive) zip_source_free(source); }
			zip_error_fini(&error); Require(archive != nullptr, "invalid-zip");
		}
		~Zip() { if (archive) zip_discard(archive); }
		Zip(const Zip&) = delete;
		Bytes Read(uint64_t index, size_t limit, bool retain = true)
		{
			zip_stat_t info{}; Require(zip_stat_index(archive, index, 0, &info) == 0 && (info.valid & ZIP_STAT_SIZE) && info.size <= limit, "zip-size-limit");
			auto* file = zip_fopen_index(archive, index, 0); Require(file != nullptr, "zip-integrity-failed");
			Bytes result; std::array<uint8_t, 65536> buffer{}; uint64_t total = 0; bool ok = true;
			for (;;)
			{
				const auto count = zip_fread(file, buffer.data(), buffer.size());
				if (!count) break;
				if (count < 0 || static_cast<uint64_t>(count) > limit - total) { ok = false; break; }
				total += count; if (retain) result.insert(result.end(), buffer.begin(), buffer.begin() + count);
			}
			ok = zip_fclose(file) == 0 && ok && total == info.size; Require(ok, "zip-integrity-failed"); return result;
		}
	};
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
	fs::path selected; Kind kind; Reader read; std::string archiveHash;
	struct Entry { fs::path path; uint64_t index = 0; fs::file_time_type time; };
	struct Pair { std::string id, account; Entry body, ext; };
	std::vector<Pair> pairs; std::vector<Candidate> candidates;
	Impl(fs::path path, Kind sourceKind, Reader reader) : selected(std::move(path)), kind(sourceKind), read(std::move(reader)) {}
	void Add(const std::map<std::wstring, Entry>& files)
	{
		for (const auto& [path, entry] : files)
		{
			if (!path.ends_with(L".ext")) continue;
			const auto slash = path.find_last_of(L'/'); const auto base = path.substr(slash == std::wstring::npos ? 0 : slash + 1, path.size() - (slash == std::wstring::npos ? 0 : slash + 1) - 4);
			if (!SaveId(base)) continue;
			const auto body = path.substr(0, path.size() - 4); auto found = files.find(body);
			Require(found == files.end() || !files.contains(body + L".wii"), "ambiguous-pair");
			if (found == files.end()) found = files.find(body + L".wii");
			std::string account;
			if (slash != std::wstring::npos)
			{
				const auto parentStart = path.rfind(L'/', slash - 1); const auto parent = path.substr(parentStart == std::wstring::npos ? 0 : parentStart + 1, slash - (parentStart == std::wstring::npos ? 0 : parentStart + 1));
				if (AccountId(parent)) account = Utf8(parent);
			}
			// タイトル情報が含まれる場合、別リージョンのフォルダを JPN と推測しない。
			CheckTitlePath(path);
			Candidate candidate; candidate.key = std::to_string(candidates.size() + 1); candidate.id = Utf8(base); candidate.account = account;
			if (found == files.end()) candidate.problem = "missing-pair";
			pairs.push_back({candidate.id, account, found != files.end() ? found->second : Entry{}, entry}); candidates.push_back(std::move(candidate));
			Require(candidates.size() <= 128, "import-count-limit");
		}
	}
	Data Load(size_t index, Zip* zip = nullptr, uint64_t* budget = nullptr, bool validate = true) const
	{
		const auto& pair = pairs.at(index); Data data;
		if (kind == Kind::Zip)
		{
			Require(zip != nullptr, "invalid-zip"); data.body = zip->Read(pair.body.index, s_bodyLimit); data.ext = zip->Read(pair.ext.index, s_extLimit);
		}
		else { data.body = read(pair.body.path, s_bodyLimit); data.ext = read(pair.ext.path, s_extLimit); }
		data.bodyTime = pair.body.time; data.extTime = pair.ext.time;
		if (budget)
		{
			const auto expanded = Read32(data.body, 4); Require(expanded <= *budget, "import-size-limit"); *budget -= expanded;
		}
		if (validate) { ValidateBody(data.body); WorldMetadata::Parse(data.ext); } return data;
	}
	void Inspect(Zip* zip = nullptr)
	{
		uint64_t budget = s_totalLimit;
		for (size_t i = 0; i < candidates.size(); ++i)
		{
			auto& candidate = candidates[i]; if (!candidate.problem.empty()) continue;
			try
			{
				const auto data = Load(i, zip, &budget); const auto meta = WorldMetadata::Parse(data.ext);
				candidate.name = meta.name; candidate.size = data.body.size() + data.ext.size(); candidate.bodyHash = Hash(data.body); candidate.extHash = Hash(data.ext);
				candidate.icon = "data:image/png;base64," + WorldMetadata::Base64(WorldMetadata::Image(data.ext));
			}
			catch (const std::exception& error)
			{
				const std::set<std::string> reasons{"unsupported-body-format","invalid-body","body-size-limit","unsupported-metadata","read-failed","unsafe-path","invalid-file","zip-integrity-failed","zip-size-limit","import-size-limit"};
				candidate.problem = reasons.contains(error.what()) ? error.what() : "invalid-world";
			}
		}
	}
	void Native()
	{
		Require(selected.is_absolute() && selected.lexically_normal() == selected, "unsafe-source-path");
		CheckTitlePath(selected.native() + L"/");
		for (auto path = selected; !path.empty(); path = path.parent_path())
		{
			const auto attributes = GetFileAttributesW(path.c_str()); Require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "unsafe-source-path");
			if (path == path.root_path()) break;
		}
		std::map<std::wstring, Entry> files; size_t visited = 0; uint64_t total = 0;
		auto add = [&](const fs::path& file, const std::wstring& relative) {
			const auto bytes = read(file, s_bodyLimit); Require(bytes.size() <= s_totalLimit - total, "import-size-limit"); total += bytes.size();
			Require(files.emplace(Lower(relative), Entry{file, 0, fs::last_write_time(file)}).second, "duplicate-source-path");
		};
		if (kind == Kind::Files)
		{
			auto base = selected; const auto extension = Lower(base.extension().native());
			if (extension == L".ext" || extension == L".wii") base.replace_extension();
			Require(SaveId(base.filename().native()), "invalid-save-id");
			const auto ext = fs::path(base.native() + L".ext"); auto body = base;
			Require(!fs::exists(body) || !fs::exists(fs::path(base.native() + L".wii")), "ambiguous-pair");
			if (!fs::exists(body)) body = fs::path(base.native() + L".wii");
			Require(fs::exists(body) && fs::exists(ext), "missing-pair");
			const auto account = Lower(base.parent_path().filename().native()); const auto prefix = AccountId(account) ? account + L"/" : L"";
			add(body, prefix + body.filename().native()); add(ext, prefix + ext.filename().native()); Add(files); Inspect(); return;
		}
		Require(fs::is_directory(selected), "invalid-source");
		std::function<void(const fs::path&, const std::wstring&, size_t)> walk;
		walk = [&](const fs::path& folder, const std::wstring& relative, size_t depth) {
			Require(depth <= 12, "import-depth-limit");
			const auto folderAttributes = GetFileAttributesW(folder.c_str()); Require(folderAttributes != INVALID_FILE_ATTRIBUTES && !(folderAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "unsafe-source-path");
			for (const auto& entry : fs::directory_iterator(folder))
			{
				Require(++visited <= 4096, "import-count-limit"); const auto name = entry.path().filename().native(); const auto lower = Lower(name);
				const auto attributes = GetFileAttributesW(entry.path().c_str()); Require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "unsafe-source-path");
				const auto child = relative.empty() ? name : relative + L"/" + name;
				if (attributes & FILE_ATTRIBUTE_DIRECTORY)
				{
					const std::set<std::wstring> structure{L"portable",L"mlc01",L"usr",L"save",L"00050000",L"101dbe00",L"user"};
					if (AccountId(lower) || structure.contains(lower)) walk(entry.path(), child, depth + 1);
				}
				else
				{
					auto id = lower; if (id.ends_with(L".ext") || id.ends_with(L".wii")) id.resize(id.size() - 4);
					if (SaveId(id)) add(entry.path(), child);
				}
			}
		};
		const auto account = Lower(selected.filename().native()); walk(selected, AccountId(account) ? account : L"", 0); Add(files); Inspect();
	}
	void Archive()
	{
		auto bytes = read(selected, s_bodyLimit); archiveHash = Hash(bytes); Zip zip(std::move(bytes));
		const auto count = zip_get_num_entries(zip.archive, 0); Require(count > 0 && count <= 4096, "import-count-limit");
		std::map<std::wstring, Entry> files; std::map<std::wstring, bool> names; uint64_t total = 0;
		for (int64_t i = 0; i < count; ++i)
		{
			zip_stat_t info{}; uint8_t os = 0; uint32_t attributes = 0;
			constexpr auto required = ZIP_STAT_NAME | ZIP_STAT_SIZE | ZIP_STAT_COMP_SIZE | ZIP_STAT_ENCRYPTION_METHOD | ZIP_STAT_COMP_METHOD;
			Require(zip_stat_index(zip.archive, i, ZIP_FL_ENC_STRICT, &info) == 0 && (info.valid & required) == required && zip_file_get_external_attributes(zip.archive, i, 0, &os, &attributes) == 0, "invalid-zip");
			const std::string_view name(info.name); const bool directory = name.ends_with('/'); const auto path = ZipPath(name);
			Require(names.emplace(path, directory).second, "duplicate-zip-path");
			const auto type = (attributes >> 16) & 0170000;
			Require((type == 0 || type == (directory ? 0040000u : 0100000u)) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) && info.encryption_method == ZIP_EM_NONE, "unsafe-zip-entry");
			Require(info.comp_method == ZIP_CM_STORE || info.comp_method == ZIP_CM_DEFLATE, "unsupported-zip-compression");
			Require(info.size <= s_bodyLimit && info.size <= s_totalLimit - total && (!info.size || (info.comp_size && info.size <= info.comp_size * 1000)), "zip-size-limit"); total += info.size;
			Require(!directory || info.size == 0, "invalid-zip");
			if (!directory)
			{
				zip.Read(i, s_bodyLimit, false);
				const auto time = (info.valid & ZIP_STAT_MTIME) ? std::chrono::clock_cast<fs::file_time_type::clock>(std::chrono::system_clock::from_time_t(info.mtime)) : fs::file_time_type::clock::now();
				files.emplace(path, Entry{{}, static_cast<uint64_t>(i), time});
			}
		}
		for (const auto& [path, directory] : names)
		{
			if (directory) continue; const auto prefix = path + L"/"; const auto next = names.lower_bound(prefix);
			Require(next == names.end() || !next->first.starts_with(prefix), "duplicate-zip-path");
		}
		Add(files); Inspect(&zip);
	}
};

WorldImport::WorldImport(fs::path selected, Kind kind, Reader reader) : m_impl(std::make_unique<Impl>(std::move(selected), kind, std::move(reader)))
{
	if (kind == Kind::Zip) m_impl->Archive(); else m_impl->Native();
	Require(!m_impl->candidates.empty(), "no-worlds");
}
WorldImport::~WorldImport() = default;
const std::vector<WorldImport::Candidate>& WorldImport::Candidates() const { return m_impl->candidates; }
WorldImport::Data WorldImport::ReadCandidate(std::string_view key) const
{
	const auto& state = *m_impl;
	const auto found = std::find_if(state.candidates.begin(), state.candidates.end(), [key](const auto& candidate) { return candidate.key == key; });
	Require(found != state.candidates.end() && found->problem.empty(), "invalid-candidate");
	const auto index = static_cast<size_t>(found - state.candidates.begin()); Data data;
	if (state.kind == Kind::Zip)
	{
		auto bytes = state.read(state.selected, s_bodyLimit); Require(Hash(bytes) == state.archiveHash, "stale-import-source"); Zip zip(std::move(bytes)); data = state.Load(index, &zip, nullptr, false);
	}
	else
	{
		data = state.Load(index, nullptr, nullptr, false);
		Require(fs::last_write_time(state.pairs[index].body.path) == state.pairs[index].body.time && fs::last_write_time(state.pairs[index].ext.path) == state.pairs[index].ext.time, "stale-import-source");
	}
	Require(Hash(data.body) == found->bodyHash && Hash(data.ext) == found->extHash, "stale-import-source");
	ValidateBody(data.body); WorldMetadata::Parse(data.ext); return data;
}
