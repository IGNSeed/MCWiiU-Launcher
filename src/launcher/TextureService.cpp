#include "TextureService.h"
#include "TextureCompatibility.h"
#include "WorldMetadata.h"

#include <Windows.h>
#include <objbase.h>
#include <openssl/sha.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <zip.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>

namespace fs = std::filesystem;
namespace
{
	using Bytes = std::vector<uint8_t>;
	using Writer = rapidjson::Writer<rapidjson::StringBuffer>;
	constexpr size_t s_fileLimit = 64 * 1024 * 1024, s_totalLimit = 256 * 1024 * 1024, s_countLimit = 1024, s_packLimit = 256;
	void Require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
	std::string Utf8(const fs::path& path) { const auto value = path.generic_u8string(); return {value.begin(), value.end()}; }
	fs::path Path(std::string_view value) { return fs::path(std::u8string(value.begin(), value.end())); }
	std::string Lower(std::string text) { for (auto& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; return text; }
	std::string Hash(std::span<const uint8_t> bytes)
	{
		std::array<uint8_t, SHA256_DIGEST_LENGTH> digest{}; SHA256(bytes.data(), bytes.size(), digest.data());
		constexpr char hex[] = "0123456789abcdef"; std::string result;
		for (const auto c : digest) { result += hex[c >> 4]; result += hex[c & 15]; } return result;
	}
	std::string Hash(std::string_view value) { return Hash({reinterpret_cast<const uint8_t*>(value.data()), value.size()}); }
	void Text(Writer& writer, const char* key, std::string_view value)
	{
		writer.Key(key); writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
	}
	std::string String(const rapidjson::Value& value)
	{
		Require(value.IsString(), "invalid-request"); std::string result(value.GetString(), value.GetStringLength());
		Require(result.find('\0') == std::string::npos, "invalid-request"); return result;
	}
	rapidjson::Document Json(std::string_view text, size_t limit = 2 * 1024 * 1024)
	{
		Require(!text.empty() && text.size() <= limit && text.find('\0') == std::string::npos, "invalid-json");
		rapidjson::Document value; value.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(), text.size());
		Require(!value.HasParseError() && value.IsObject(), "invalid-json"); return value;
	}
	void Keys(const rapidjson::Value& value, std::initializer_list<const char*> keys)
	{
		Require(value.IsObject() && value.MemberCount() == keys.size(), "invalid-json"); std::set<std::string> seen;
		for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) Require(seen.insert(String(it->name)).second, "invalid-json");
		for (const auto key : keys) Require(value.HasMember(key), "invalid-json");
	}
	std::string Guid()
	{
		GUID value{}; Require(SUCCEEDED(CoCreateGuid(&value)), "random-failed");
		return Hash({reinterpret_cast<const uint8_t*>(&value), sizeof(value)});
	}
	bool Digest(std::string_view value) { return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string_view::npos; }
	void CheckPath(fs::path path)
	{
		Require(path.is_absolute() && path.lexically_normal() == path && path.has_root_name(), "unsafe-path");
		for (; !path.empty(); path = path.parent_path())
		{
			const auto attr = GetFileAttributesW(path.c_str());
			if (attr == INVALID_FILE_ATTRIBUTES) Require(GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND, "unsafe-path");
			else Require(!(attr & FILE_ATTRIBUTE_REPARSE_POINT), "unsafe-path");
			if (path == path.root_path()) break;
		}
	}
	struct Handle
	{
		HANDLE value = INVALID_HANDLE_VALUE;
		~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
		Handle() = default; Handle(const Handle&) = delete;
	};
	struct Guard
	{
		std::vector<HANDLE> handles;
		explicit Guard(fs::path path)
		{
			try
			{
				CheckPath(path);
				for (; !path.empty(); path = path.parent_path())
				{
					if (fs::is_directory(path))
					{
						const auto file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
						Require(file != INVALID_HANDLE_VALUE, "unsafe-path"); handles.push_back(file);
						BY_HANDLE_FILE_INFORMATION info{}; Require(GetFileInformationByHandle(file, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "unsafe-path");
					}
					if (path == path.root_path()) break;
				}
			}
			catch (...) { for (auto file : handles) CloseHandle(file); throw; }
		}
		~Guard() { for (auto file : handles) CloseHandle(file); }
	};
	struct File
	{
		Guard directories;
		Handle handle;
		BY_HANDLE_FILE_INFORMATION info{};
		explicit File(const fs::path& path) : directories(path.parent_path())
		{
			CheckPath(path);
			handle.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
			Require(handle.value != INVALID_HANDLE_VALUE, "file-busy-or-missing");
			Require(GetFileInformationByHandle(handle.value, &info) && !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) && info.nNumberOfLinks == 1, "unsafe-file");
		}
		Bytes Read(size_t limit)
		{
			const uint64_t length = uint64_t(info.nFileSizeHigh) << 32 | info.nFileSizeLow;
			Require(length <= limit, "file-size-limit"); Bytes bytes(static_cast<size_t>(length));
			LARGE_INTEGER start{}; Require(SetFilePointerEx(handle.value, start, nullptr, FILE_BEGIN), "read-failed");
			size_t offset = 0;
			while (offset < bytes.size())
			{
				DWORD count = 0; Require(ReadFile(handle.value, bytes.data() + offset, static_cast<DWORD>(std::min<size_t>(1024 * 1024, bytes.size() - offset)), &count, nullptr) && count, "read-failed"); offset += count;
			}
			BY_HANDLE_FILE_INFORMATION after{};
			Require(GetFileInformationByHandle(handle.value, &after) && !memcmp(&info.ftLastWriteTime, &after.ftLastWriteTime, sizeof(FILETIME)) && after.nFileSizeLow == info.nFileSizeLow && after.nFileSizeHigh == info.nFileSizeHigh && after.nNumberOfLinks == 1, "source-changed");
			return bytes;
		}
	};
	Bytes Read(const fs::path& path, size_t limit = s_fileLimit) { return File(path).Read(limit); }
	std::string ReadText(const fs::path& path, size_t limit = 2 * 1024 * 1024)
	{
		const auto bytes = Read(path, limit); return {bytes.begin(), bytes.end()};
	}
	void CreateDirectory(const fs::path& path)
	{
		CheckPath(path); Guard guard(path.parent_path());
		fs::create_directories(path); CheckPath(path); Require(fs::is_directory(path), "write-failed");
	}
	void WriteNew(const fs::path& path, std::span<const uint8_t> bytes)
	{
		Guard guard(path.parent_path()); CheckPath(path); Handle file;
		file.value = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
		Require(file.value != INVALID_HANDLE_VALUE, "write-failed"); size_t offset = 0;
		while (offset < bytes.size())
		{
			DWORD count = 0; Require(WriteFile(file.value, bytes.data() + offset, static_cast<DWORD>(std::min<size_t>(1024 * 1024, bytes.size() - offset)), &count, nullptr) && count, "write-failed"); offset += count;
		}
		Require(FlushFileBuffers(file.value), "write-failed");
	}
	void WriteNew(const fs::path& path, std::string_view text) { WriteNew(path, {reinterpret_cast<const uint8_t*>(text.data()), text.size()}); }
	void AtomicWrite(const fs::path& path, std::string_view text)
	{
		Guard guard(path.parent_path()); CheckPath(path);
		const auto temp = path.parent_path() / Path("write-" + Guid() + ".json"); WriteNew(temp, text);
		Require(MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "commit-failed");
		Require(ReadText(path) == text, "commit-verify-failed");
	}
	void Relative(std::string_view path, bool directory = false)
	{
		Require(!path.empty() && path.size() <= 240 && path.front() != '/' && path.find('\\') == std::string_view::npos && path.find(':') == std::string_view::npos && path.find('\0') == std::string_view::npos, "unsafe-pack-path");
		std::string normalized(path); if (directory && normalized.back() == '/') normalized.pop_back();
		size_t offset = 0;
		while (offset < normalized.size())
		{
			auto next = normalized.find('/', offset); if (next == std::string::npos) next = normalized.size();
			const auto part = normalized.substr(offset, next - offset);
			Require(!part.empty() && part != "." && part != ".." && part.back() != '.' && part.back() != ' ' && part.find_first_of("<>\"|?*") == std::string::npos && std::none_of(part.begin(), part.end(), [](unsigned char c) { return c < 32; }), "unsafe-pack-path");
			const auto stem = Lower(part.substr(0, part.find('.')));
			Require(stem != "con" && stem != "prn" && stem != "aux" && stem != "nul" && !(stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '0' && stem[3] <= '9'), "unsafe-pack-path");
			offset = next + 1;
		}
		Require(directory || normalized.back() != '/', "unsafe-pack-path");
		Require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, normalized.data(), static_cast<int>(normalized.size()), nullptr, 0) > 0, "unsafe-pack-path");
	}
	std::string Description(const Bytes& bytes)
	{
		if (bytes.empty()) return {};
		const auto count = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), nullptr, 0);
		std::wstring wide(count, 0); MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), wide.data(), count);
		if (!wide.empty() && wide.front() == 0xfeff) wide.erase(0, 1);
		for (auto& c : wide) if (c < 32 && c != '\n' && c != '\r' && c != '\t') c = 0xfffd;
		const auto length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
		std::string result(length, 0); WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), length, nullptr, nullptr); return result;
	}
	struct Resource { std::string path, hash; size_t size; Bytes bytes; };
	struct Pack
	{
		std::string id = "default", name = "Default", kind = "default", source, fingerprint, icon, description = "Original Minecraft textures", problem;
		std::vector<Resource> files;
	};
	std::string Fingerprint(const Pack& pack)
	{
		std::string data;
		for (const auto& file : pack.files) data += file.path + '\n' + std::to_string(file.size) + '\n' + file.hash + '\n';
		return Hash(data);
	}
	bool Metadata(std::string_view path) { return path == "Common/res/description.txt" || path == "Common/res/gui/pack_icon.png"; }
	void PackJson(Writer& writer, const Pack& pack)
	{
		writer.StartObject(); Text(writer, "id", pack.id); Text(writer, "name", pack.name); Text(writer, "kind", pack.kind); Text(writer, "source", pack.source);
		Text(writer, "fingerprint", pack.fingerprint); Text(writer, "description", pack.description); Text(writer, "icon", pack.icon); Text(writer, "problem", pack.problem);
		writer.Key("fileCount"); writer.Uint64(std::count_if(pack.files.begin(), pack.files.end(), [](const auto& file) { return !Metadata(file.path); })); writer.EndObject();
	}
} // namespace

struct TextureService::Impl
{
	fs::path portable, root, storage, base, update;
	std::mutex mutex;
	std::atomic_bool healthy{false};
	std::vector<Pack> packs;
	Pack current, preview;
	std::string state = "error", notice, token, loadedId, loadedName;
	std::chrono::steady_clock::time_point expires{};
	uint64_t revision = 0;
	bool running = false;
	std::vector<std::unique_ptr<File>> pins;
	std::unique_ptr<Guard> storageGuard;

	Impl(fs::path config, fs::path baseCommon, fs::path updateCommon) : portable(std::move(config)), root(portable / "Textures"), storage(portable / ".launcher-textures"), base(std::move(baseCommon)), update(std::move(updateCommon))
	{
		try { Guard guard(portable); CreateDirectory(root); CreateDirectory(storage / "generations"); LoadCurrent(false); }
		catch (const std::exception& error) { notice = error.what(); }
	}

	Bytes Vanilla(std::string_view path)
	{
		const auto relative = Path(path.substr(7));
		CheckPath(update / relative); CheckPath(base / relative);
		if (fs::exists(update / relative)) return Read(update / relative);
		if (fs::exists(base / relative)) return Read(base / relative);
		return {};
	}
	void Validate(Pack& pack)
	{
		Require(!pack.files.empty(), "empty-pack");
		for (auto& file : pack.files)
		{
			file.hash = Hash(file.bytes); file.size = file.bytes.size();
			if (file.path == "Common/res/description.txt") { Require(file.bytes.size() <= 16384, "description-size-limit"); pack.description = Description(file.bytes); }
			else if (file.path == "Common/res/gui/pack_icon.png")
			{
				try { Require(file.bytes.size() <= 4 * 1024 * 1024, "icon-size-limit"); pack.icon = "data:image/png;base64," + WorldMetadata::Base64(TextureCompatibility::Icon(file.bytes)); }
				catch (const std::exception&) { pack.icon.clear(); }
			}
			else
			{
				const auto vanilla = Vanilla(file.path);
				if (file.path.ends_with(".txt")) Require(!vanilla.empty() && file.bytes == vanilla, "unverified-sequence");
				else TextureCompatibility::Resource(file.path, file.bytes, vanilla);
			}
		}
		const std::string terrain = "Common/res/TitleUpdate/res/terrain";
		const auto find = [&](const std::string& path) -> const Resource* { for (const auto& file : pack.files) if (file.path == path) return &file; return nullptr; };
		const auto atlas = find(terrain + ".png"), mip2 = find(terrain + "MipMapLevel2.png"), mip3 = find(terrain + "MipMapLevel3.png");
		if (atlas || mip2 || mip3)
		{
			Require(atlas && mip2 && mip3, "terrain-mipmaps-required");
			const auto size = TextureCompatibility::Png(atlas->bytes), a = TextureCompatibility::Png(mip2->bytes), b = TextureCompatibility::Png(mip3->bytes);
			Require(size.first == a.first * 2 && size.second == a.second * 2 && size.first == b.first * 4 && size.second == b.second * 4, "terrain-mipmap-size");
		}
		pack.fingerprint = Fingerprint(pack);
	}
	Pack ReadPack(const Pack& selected)
	{
		Pack result; result.id = selected.id; result.kind = selected.kind; result.name = selected.name; result.source = selected.source; result.description.clear();
		Relative(result.source); Require(Path(result.source).filename() == Path(result.source), "unsafe-pack-path");
		const auto sourceName = Path(result.source);
		Require(selected.kind == "folder" || (selected.kind == "zip" && Lower(Utf8(sourceName.extension())) == ".zip"), "invalid-pack");
		result.name = Utf8(selected.kind == "folder" ? sourceName : sourceName.stem());
		const auto path = root / Path(result.source); Guard guard(root); CheckPath(path);
		std::vector<std::unique_ptr<File>> sourcePins;
		size_t total = 0; std::set<std::string> seen;
		auto add = [&](std::string name, Bytes bytes) {
			Relative(name); Require(name.starts_with("Common/"), "common-root-required");
			Require(seen.insert(Lower(name)).second, "duplicate-entry"); total += bytes.size();
			Require(result.files.size() < s_countLimit && total <= s_totalLimit, "pack-size-limit"); result.files.push_back({std::move(name), {}, bytes.size(), std::move(bytes)});
		};
		if (selected.kind == "folder")
		{
			Guard packGuard(path); CheckPath(path / "Common"); Require(fs::is_directory(path / "Common"), "common-root-required");
			size_t entries = 0;
			for (const auto& entry : fs::recursive_directory_iterator(path / "Common"))
			{
				Require(++entries <= s_countLimit * 4, "pack-size-limit");
				CheckPath(entry.path()); const auto relative = Utf8(entry.path().lexically_relative(path)); Relative(relative, entry.is_directory());
				if (entry.is_directory()) continue;
				Require(entry.is_regular_file(), "unsafe-file"); auto pin = std::make_unique<File>(entry.path());
				add(relative, pin->Read(s_fileLimit)); sourcePins.push_back(std::move(pin));
			}
			std::set<std::string> verified;
			for (const auto& entry : fs::recursive_directory_iterator(path / "Common"))
			{
				CheckPath(entry.path()); if (!entry.is_directory()) verified.insert(Lower(Utf8(entry.path().lexically_relative(path))));
			}
			Require(verified == seen, "source-changed");
		}
		else
		{
			Require(selected.kind == "zip", "invalid-pack"); auto pin = std::make_unique<File>(path); const auto bytes = pin->Read(s_totalLimit); sourcePins.push_back(std::move(pin));
			zip_error_t error; zip_error_init(&error); const auto source = zip_source_buffer_create(bytes.data(), bytes.size(), 0, &error); zip_error_fini(&error);
			Require(source != nullptr, "invalid-zip");
			auto archive = zip_open_from_source(source, ZIP_RDONLY | ZIP_CHECKCONS, nullptr);
			if (!archive) { zip_source_free(source); throw std::runtime_error("invalid-zip"); }
			struct ZipGuard { zip_t* archive; ~ZipGuard() { zip_discard(archive); } } zipGuard{archive};
			const auto count = zip_get_num_entries(archive, 0); Require(count > 0 && count <= s_countLimit * 2, "zip-count-limit");
			std::set<std::string> entries;
			for (zip_int64_t i = 0; i < count; ++i)
			{
				zip_stat_t stat{}; Require(zip_stat_index(archive, i, ZIP_FL_ENC_STRICT, &stat) == 0 && stat.name, "invalid-zip");
				std::string name(stat.name); const bool directory = !name.empty() && name.back() == '/'; Relative(name, directory);
				Require(name == "Common/" || name.starts_with("Common/"), "common-root-required");
				std::string key = Lower(name); if (directory) key.pop_back(); Require(entries.insert(key).second, "duplicate-entry");
				zip_uint8_t system = 0; zip_uint32_t attributes = 0;
				Require(zip_file_get_external_attributes(archive, i, 0, &system, &attributes) == 0 && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
					(system != ZIP_OPSYS_UNIX || ((attributes >> 16) & 0170000U) == 0 || ((attributes >> 16) & 0170000U) == (directory ? 0040000U : 0100000U)), "zip-link-rejected");
				Require(stat.encryption_method == ZIP_EM_NONE && (stat.comp_method == ZIP_CM_STORE || stat.comp_method == ZIP_CM_DEFLATE) && stat.size <= s_fileLimit &&
					(!stat.size || (stat.comp_size && stat.size <= stat.comp_size * 1000)), "zip-size-limit");
				if (directory) { Require(stat.size == 0, "invalid-zip"); continue; }
				Require(total + stat.size <= s_totalLimit, "pack-size-limit");
				const auto file = zip_fopen_index(archive, i, 0); Require(file != nullptr, "invalid-zip"); Bytes data(static_cast<size_t>(stat.size)); size_t offset = 0;
				bool ok = true;
				while (offset < data.size()) { const auto n = zip_fread(file, data.data() + offset, data.size() - offset); if (n <= 0) { ok = false; break; } offset += static_cast<size_t>(n); }
				uint8_t extra = 0; ok = ok && zip_fread(file, &extra, 1) == 0; const auto closed = zip_fclose(file);
				Require(ok && closed == 0 && crc32(0, data.data(), static_cast<uInt>(data.size())) == stat.crc, "zip-crc"); add(std::move(name), std::move(data));
			}
		}
		std::sort(result.files.begin(), result.files.end(), [](const auto& a, const auto& b) { return a.path < b.path; }); Validate(result); return result;
	}
	void Scan()
	{
		Guard guard(root); packs.clear(); packs.emplace_back();
		for (const auto& entry : fs::directory_iterator(root))
		{
			Require(packs.size() <= s_packLimit, "pack-count-limit"); Pack pack; pack.source = Utf8(entry.path().filename()); pack.description.clear();
			pack.kind = entry.is_directory() ? "folder" : "zip";
			if (pack.kind == "zip" && Lower(Utf8(entry.path().extension())) != ".zip") continue;
			pack.name = Utf8(pack.kind == "folder" ? entry.path().filename() : entry.path().stem());
			pack.id = pack.kind + ":" + Hash(pack.source);
			try { pack = ReadPack(pack); for (auto& file : pack.files) { file.bytes.clear(); file.bytes.shrink_to_fit(); } }
			catch (const std::exception& error) { pack.problem = error.what(); pack.files.clear(); }
			packs.push_back(std::move(pack));
		}
		std::sort(packs.begin() + 1, packs.end(), [](const auto& a, const auto& b) { return std::pair{Lower(a.name), a.id} < std::pair{Lower(b.name), b.id}; });
	}
	std::string Manifest(const Pack& pack)
	{
		rapidjson::StringBuffer buffer; Writer writer(buffer); writer.StartObject(); writer.Key("schema"); writer.Uint(1); writer.Key("pack"); PackJson(writer, pack);
		writer.Key("files"); writer.StartArray();
		for (const auto& file : pack.files) { writer.StartObject(); Text(writer, "path", file.path); Text(writer, "sha256", file.hash); writer.Key("size"); writer.Uint64(file.size); writer.EndObject(); }
		writer.EndArray(); writer.EndObject(); return buffer.GetString();
	}
	std::string Pointer(std::string_view generation, std::string_view manifestHash)
	{
		rapidjson::StringBuffer buffer; Writer writer(buffer); writer.StartObject(); writer.Key("schema"); writer.Uint(1); Text(writer, "generation", generation); Text(writer, "manifestHash", manifestHash); writer.EndObject(); return buffer.GetString();
	}
	fs::path Generation()
	{
		const auto pointer = Json(ReadText(storage / "active.json")); Keys(pointer, {"schema", "generation", "manifestHash"});
		Require(pointer["schema"].IsUint() && pointer["schema"].GetUint() == 1 && Digest(String(pointer["generation"])) && Digest(String(pointer["manifestHash"])), "invalid-manifest");
		const auto path = storage / "generations" / Path(String(pointer["generation"]));
		Require(Hash(ReadText(path / "manifest.json")) == String(pointer["manifestHash"]), "manifest-hash-mismatch"); return path;
	}
	void LoadCurrent(bool sourceCheck)
	{
		healthy = false; current = Pack{}; state = "error";
		try
		{
			Guard guard(storage); CheckPath(storage / "transaction.json");
			if (fs::exists(storage / "transaction.json")) { state = "recovery-required"; return; }
			CheckPath(storage / "active.json");
			if (!fs::exists(storage / "active.json"))
			{
				Require(fs::is_empty(storage / "generations"), "selection-missing"); state = "default"; healthy = true; return;
			}
			const auto path = Generation(); const auto manifest = Json(ReadText(path / "manifest.json")); Keys(manifest, {"schema", "pack", "files"});
			Require(manifest["schema"].IsUint() && manifest["schema"].GetUint() == 1 && manifest["files"].IsArray() && manifest["files"].Size() <= s_countLimit, "invalid-manifest");
			const auto& p = manifest["pack"]; Keys(p, {"id", "name", "kind", "source", "fingerprint", "description", "icon", "problem", "fileCount"});
			current.id = String(p["id"]); current.name = String(p["name"]); current.kind = String(p["kind"]); current.source = String(p["source"]); current.description = String(p["description"]); current.icon = String(p["icon"]); current.fingerprint = String(p["fingerprint"]);
			std::set<std::string> seen; size_t total = 0;
			for (const auto& item : manifest["files"].GetArray())
			{
				Keys(item, {"path", "sha256", "size"}); const auto name = String(item["path"]), hash = String(item["sha256"]); Relative(name);
				Require(name.starts_with("Common/") && Digest(hash) && item["size"].IsUint64() && item["size"].GetUint64() <= s_fileLimit && seen.insert(Lower(name)).second, "invalid-manifest");
				const auto size = static_cast<size_t>(item["size"].GetUint64()); total += size; Require(total <= s_totalLimit, "pack-size-limit");
				const auto bytes = Read(path / Path(name)); Require(bytes.size() == size && Hash(bytes) == hash, "snapshot-hash-mismatch"); current.files.push_back({name, hash, size, {}});
			}
			std::set<std::string> expected{"manifest.json"}; for (const auto& file : current.files) expected.insert(file.path);
			for (const auto& entry : fs::recursive_directory_iterator(path)) { CheckPath(entry.path()); if (!entry.is_directory()) Require(expected.erase(Utf8(entry.path().lexically_relative(path))) == 1, "unknown-snapshot-file"); }
			Require(expected.empty(), "snapshot-file-missing");
			if (current.kind == "default")
			{
				Require(current.id == "default" && current.files.empty() && current.source.empty(), "invalid-manifest"); current = Pack{};
			}
			else
			{
				Require((current.kind == "folder" || current.kind == "zip") && current.id == current.kind + ":" + Hash(current.source) && current.fingerprint == Fingerprint(current), "invalid-manifest");
				if (sourceCheck)
				{
					const auto source = ReadPack(current);
					if (source.fingerprint != current.fingerprint) { state = "external-unknown"; notice = "source-changed"; return; }
					current.name = source.name; current.icon = source.icon; current.description = source.description;
				}
			}
			state = current.kind == "default" ? "default" : "known-pack"; healthy = true;
		}
		catch (const std::exception& error) { notice = error.what(); if (notice == "selection-missing" || notice == "source-changed" || notice == "file-busy-or-missing") state = "external-unknown"; }
	}
	void Apply(const Pack& pack, bool recovery)
	{
		Guard guard(storage); Require(!running, "game-running");
		if (!recovery) Require(healthy, "recovery-required");
		const auto generation = Guid(); const auto next = storage / "generations" / Path(generation);
		// 旧世代は読み取り専用の復旧元として残し、別世代のファイルを混ぜない。
		uint64_t stored = 0;
		for (const auto& entry : fs::recursive_directory_iterator(storage / "generations")) { CheckPath(entry.path()); if (entry.is_regular_file()) stored += entry.file_size(); }
		Require(pack.id == "default" || stored <= 2ULL * 1024 * 1024 * 1024, "snapshot-storage-limit");
		std::string previous;
		CheckPath(storage / "active.json"); if (fs::exists(storage / "active.json")) previous = ReadText(storage / "active.json");
		if (recovery && fs::exists(storage / "transaction.json"))
		{
			CheckPath(storage / "transaction.json"); const auto archive = storage / Path("interrupted-" + Guid() + ".json");
			Require(MoveFileExW((storage / "transaction.json").c_str(), archive.c_str(), MOVEFILE_WRITE_THROUGH), "recovery-failed");
		}
		WriteNew(storage / "transaction.json", "{\"schema\":1,\"operation\":\"texture-apply\"}"); healthy = false;
		try
		{
			CreateDirectory(next);
			for (const auto& file : pack.files)
			{
				const auto target = next / Path(file.path); CreateDirectory(target.parent_path()); WriteNew(target, file.bytes);
				Require(Hash(Read(target)) == file.hash, "copy-hash-mismatch");
			}
			const auto manifest = Manifest(pack); WriteNew(next / "manifest.json", manifest);
			if (!previous.empty()) AtomicWrite(storage / "previous.json", previous);
			AtomicWrite(storage / "active.json", Pointer(generation, Hash(manifest)));
			Require(Generation() == next, "commit-verify-failed");
			Require(DeleteFileW((storage / "transaction.json").c_str()), "commit-finalize-failed");
			LoadCurrent(false); Require(healthy, "commit-verify-failed"); notice = recovery ? "recovered" : "applied";
		}
		catch (...)
		{
			try
			{
				if (!previous.empty()) AtomicWrite(storage / "active.json", previous);
				else if (fs::exists(storage / "active.json")) Require(DeleteFileW((storage / "active.json").c_str()), "rollback-failed");
				// 初回中断は残した世代を含めて明示的な Default 復旧を要求する。
				if (!previous.empty()) { Require(DeleteFileW((storage / "transaction.json").c_str()), "rollback-failed"); LoadCurrent(false); }
			}
			catch (...) { healthy = false; state = "recovery-required"; }
			if (!healthy) state = "recovery-required";
			throw;
		}
	}
	std::string Snapshot(bool canEdit)
	{
		rapidjson::StringBuffer buffer; Writer writer(buffer); writer.StartObject();
		Text(writer, "notice", notice); Text(writer, "state", state); writer.Key("current"); PackJson(writer, current);
		writer.Key("revision"); writer.Uint64(revision); writer.Key("canEdit"); writer.Bool(canEdit && !running && healthy);
		writer.Key("canRecover"); writer.Bool(canEdit && !running && !healthy); writer.Key("running"); writer.Bool(running);
		Text(writer, "loadedId", loadedId); Text(writer, "loadedName", loadedName); Text(writer, "token", token);
		writer.Key("preview"); if (!token.empty()) PackJson(writer, preview); else writer.Null();
		writer.Key("packs"); writer.StartArray(); for (const auto& pack : packs) PackJson(writer, pack); writer.EndArray(); writer.EndObject(); return buffer.GetString();
	}
};

TextureService::TextureService(fs::path portable, fs::path baseCommon, fs::path updateCommon) : m_impl(std::make_unique<Impl>(std::move(portable), std::move(baseCommon), std::move(updateCommon))) {}
TextureService::~TextureService() = default;
bool TextureService::IsHealthy() const { return m_impl->healthy; }

std::string TextureService::Command(std::string_view payload, bool canEdit)
{
	auto& s = *m_impl; std::lock_guard lock(s.mutex); s.notice.clear();
	try
	{
		const auto json = Json(payload, 8192); Require(json.HasMember("op"), "invalid-request"); const auto op = String(json["op"]);
		if (op == "list") { Keys(json, {"op"}); s.token.clear(); ++s.revision; s.Scan(); s.LoadCurrent(!s.running); }
		else if (op == "cancel") { Keys(json, {"op"}); s.token.clear(); }
		else
		{
			Require(canEdit && !s.running, "game-running");
			if (op == "recover") { Keys(json, {"op"}); s.Apply(Pack{}, true); s.token.clear(); ++s.revision; s.Scan(); }
			else
			{
				s.LoadCurrent(true); Require(s.healthy, "recovery-required");
				if (op == "preview")
				{
					Keys(json, {"op", "id", "revision"}); Require(json["revision"].IsUint64() && json["revision"].GetUint64() == s.revision, "stale-preview");
					const auto id = String(json["id"]); const auto pack = std::find_if(s.packs.begin(), s.packs.end(), [&](const auto& p) { return p.id == id; });
					Require(pack != s.packs.end() && pack->problem.empty(), "invalid-pack"); s.preview = id == "default" ? Pack{} : s.ReadPack(*pack);
					Require(id == "default" || s.preview.fingerprint == pack->fingerprint, "source-changed"); s.token = Guid(); s.expires = std::chrono::steady_clock::now() + std::chrono::minutes(5); s.notice = "preview-ready";
					for (auto& file : s.preview.files) { file.bytes.clear(); file.bytes.shrink_to_fit(); }
				}
				else if (op == "apply")
				{
					Keys(json, {"op", "token"}); Require(!s.token.empty() && String(json["token"]) == s.token && std::chrono::steady_clock::now() < s.expires, "stale-preview");
					const auto pack = s.preview.id == "default" ? Pack{} : s.ReadPack(s.preview); Require(pack.fingerprint == s.preview.fingerprint, "source-changed");
					s.token.clear(); s.Apply(pack, false); ++s.revision; s.Scan();
				}
				else throw std::runtime_error("invalid-request");
			}
		}
	}
	catch (const std::exception& error) { s.notice = error.what(); s.token.clear(); }
	if (s.token.empty()) s.preview = Pack{};
	return s.Snapshot(canEdit);
}

std::string TextureService::OpenFolder(const std::function<bool(const fs::path&)>& open)
{
	auto& s = *m_impl; std::lock_guard lock(s.mutex);
	try { Guard guard(s.portable); CreateDirectory(s.root); Guard pinned(s.root); Require(open(s.root), "folder-open-failed"); s.notice = "folder-opened"; }
	catch (const std::exception& error) { s.notice = error.what(); }
	return s.Snapshot(true);
}

std::vector<TextureService::OverlayFile> TextureService::BeginTitle(uint64_t titleId, uint16_t version)
{
	auto& s = *m_impl; std::lock_guard lock(s.mutex);
	Require(!s.running && titleId == 0x00050000101dbe00ULL && version == 560, "texture-title-mismatch");
	s.LoadCurrent(true); Require(s.healthy, "texture-recovery-required");
	std::vector<OverlayFile> result;
	try
	{
		if (s.current.id != "default")
		{
			const auto generation = s.Generation(); s.storageGuard = std::make_unique<Guard>(generation);
			for (const auto& file : s.current.files)
			{
				const auto path = generation / Path(file.path); auto pin = std::make_unique<File>(path);
				Require(Hash(pin->Read(s_fileLimit)) == file.hash, "snapshot-hash-mismatch"); s.pins.push_back(std::move(pin));
				if (!Metadata(file.path)) result.push_back({"/vol/content/" + file.path, file.hash, path, file.size});
			}
		}
		s.running = true; s.loadedId = s.current.id; s.loadedName = s.current.name; s.token.clear(); s.preview = Pack{}; return result;
	}
	catch (...) { s.pins.clear(); s.storageGuard.reset(); s.healthy = false; throw; }
}

void TextureService::EndTitle()
{
	auto& s = *m_impl; std::lock_guard lock(s.mutex); s.pins.clear(); s.storageGuard.reset(); s.running = false; s.loadedId.clear(); s.loadedName.clear();
}
