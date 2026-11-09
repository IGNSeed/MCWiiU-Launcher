#include "WorldService.h"
#include "WorldMetadata.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <openssl/sha.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;
namespace
{
	using Bytes = WorldMetadata::Bytes;
	using Writer = rapidjson::Writer<rapidjson::StringBuffer>;
	constexpr size_t s_bodyLimit = 512 * 1024 * 1024, s_extLimit = 1024 * 1024;
	void Require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
	struct Handle
	{
		HANDLE value = INVALID_HANDLE_VALUE;
		~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
		Handle() = default;
		Handle(const Handle&) = delete;
		Handle& operator=(const Handle&) = delete;
	};
	std::string Hex(uint32_t id)
	{
		char result[9]{}; snprintf(result, sizeof(result), "%08x", id); return result;
	}
	bool SaveId(std::string_view value)
	{
		return value.size() == 12 && std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
	}
	void CheckPath(fs::path path)
	{
		Require(path.is_absolute() && path.lexically_normal() == path && path.has_root_name(), "unsafe-path");
		for (; !path.empty(); path = path.parent_path())
		{
			const auto attributes = GetFileAttributesW(path.c_str());
			if (attributes == INVALID_FILE_ATTRIBUTES)
				Require(GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND, "unsafe-path");
			else Require(!(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "unsafe-path");
			if (path == path.root_path()) break;
		}
	}
	struct DirectoryGuard
	{
		std::vector<HANDLE> handles;
		explicit DirectoryGuard(fs::path path)
		{
			try
			{
				CheckPath(path);
				for (; !path.empty(); path = path.parent_path())
				{
					if (fs::is_directory(path))
					{
						const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
						Require(handle != INVALID_HANDLE_VALUE, "unsafe-path"); handles.push_back(handle);
						BY_HANDLE_FILE_INFORMATION info{};
						Require(GetFileInformationByHandle(handle, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "unsafe-path");
					}
					if (path == path.root_path()) break;
				}
			}
			catch (...) { for (const auto handle : handles) CloseHandle(handle); throw; }
		}
		~DirectoryGuard() { for (const auto handle : handles) CloseHandle(handle); }
		DirectoryGuard(const DirectoryGuard&) = delete;
	};
	Bytes Read(const fs::path& path, size_t limit)
	{
		CheckPath(path);
		Handle file;
		file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		Require(file.value != INVALID_HANDLE_VALUE, "read-failed");
		BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER length{};
		Require(GetFileInformationByHandle(file.value, &info) && !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
			info.nNumberOfLinks == 1 && GetFileSizeEx(file.value, &length) && length.QuadPart > 0 && static_cast<uint64_t>(length.QuadPart) <= limit, "invalid-file");
		Bytes bytes(static_cast<size_t>(length.QuadPart));
		size_t offset = 0;
		while (offset < bytes.size())
		{
			DWORD count = 0;
			Require(ReadFile(file.value, bytes.data() + offset, static_cast<DWORD>(std::min<size_t>(1024 * 1024, bytes.size() - offset)), &count, nullptr) && count, "read-failed");
			offset += count;
		}
		return bytes;
	}
	void WriteNew(const fs::path& path, std::span<const uint8_t> bytes)
	{
		CheckPath(path);
		Handle file;
		file.value = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
		Require(file.value != INVALID_HANDLE_VALUE, "write-failed");
		size_t offset = 0;
		while (offset < bytes.size())
		{
			DWORD count = 0;
			Require(WriteFile(file.value, bytes.data() + offset, static_cast<DWORD>(std::min<size_t>(1024 * 1024, bytes.size() - offset)), &count, nullptr) && count, "write-failed");
			offset += count;
		}
		Require(FlushFileBuffers(file.value), "write-failed");
	}
	void WriteText(const fs::path& path, std::string_view text)
	{
		WriteNew(path, {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
	}
	bool Guid(std::string_view id)
	{
		if (id.size() != 38 || id.front() != '{' || id.back() != '}') return false;
		for (size_t i = 1; i < 37; ++i)
		{
			if (i == 9 || i == 14 || i == 19 || i == 24) { if (id[i] != '-') return false; }
			else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f') || (id[i] >= 'A' && id[i] <= 'F'))) return false;
		}
		return true;
	}
	std::string NewGuid()
	{
		GUID guid{}; Require(SUCCEEDED(CoCreateGuid(&guid)), "write-failed");
		wchar_t identifier[40]{}; StringFromGUID2(guid, identifier, 40);
		return fs::path(identifier).string();
	}
	int64_t NowMilliseconds()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	}
	void Receipt(const fs::path& path, std::string_view expected)
	{
		if (!fs::exists(path)) WriteText(path, expected);
		else
		{
			const auto value = Read(path, 128); Require(std::string_view(reinterpret_cast<const char*>(value.data()), value.size()) == expected, "recovery-required");
		}
	}
	std::string Hash(std::span<const uint8_t> bytes)
	{
		std::array<uint8_t, SHA256_DIGEST_LENGTH> hash{};
		SHA256(bytes.data(), bytes.size(), hash.data());
		std::string result;
		constexpr char hex[] = "0123456789abcdef";
		for (const auto b : hash) { result += hex[b >> 4]; result += hex[b & 15]; }
		return result;
	}
	void Text(Writer& writer, const char* key, std::string_view value)
	{
		writer.Key(key); writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
	}
	std::string String(const rapidjson::Value& value)
	{
		Require(value.IsString(), "invalid-request");
		std::string result(value.GetString(), value.GetStringLength());
		Require(result.find('\0') == std::string::npos, "invalid-request"); return result;
	}
	rapidjson::Document Json(std::string_view payload, size_t limit = 8192)
	{
		Require(!payload.empty() && payload.size() <= limit && payload.find('\0') == std::string_view::npos, "invalid-request");
		rapidjson::Document json; json.Parse<rapidjson::kParseValidateEncodingFlag>(payload.data(), payload.size());
		Require(!json.HasParseError() && json.IsObject(), "invalid-request");
		std::set<std::string> names;
		for (auto it = json.MemberBegin(); it != json.MemberEnd(); ++it) Require(names.insert(String(it->name)).second, "invalid-request");
		return json;
	}
	std::string JsonText(const rapidjson::Value& value)
	{
		rapidjson::StringBuffer buffer; Writer writer(buffer); value.Accept(writer); return buffer.GetString();
	}
	void Keys(const rapidjson::Value& value, std::initializer_list<const char*> keys)
	{
		Require(value.IsObject() && value.MemberCount() == keys.size(), "invalid-purge");
		std::set<std::string> names;
		for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) Require(names.insert(String(it->name)).second, "invalid-purge");
		for (const auto key : keys) Require(value.HasMember(key), "invalid-purge");
	}
	struct PurgeHandle
	{
		Handle file;
		std::string identity;
		uint64_t size = 0;
		bool isDirectory;
		PurgeHandle(const fs::path& path, bool directory, bool deleting) : isDirectory(directory)
		{
			CheckPath(path);
			file.value = CreateFileW(path.c_str(), (directory ? FILE_READ_ATTRIBUTES : GENERIC_READ) | (deleting ? DELETE : 0),
				directory ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ, nullptr, OPEN_EXISTING,
				FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_FLAG_SEQUENTIAL_SCAN), nullptr);
			Require(file.value != INVALID_HANDLE_VALUE, "purge-file-busy");
			BY_HANDLE_FILE_INFORMATION info{};
			Require(GetFileInformationByHandle(file.value, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
				bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory && (directory || info.nNumberOfLinks == 1) &&
				!(info.dwFileAttributes & FILE_ATTRIBUTE_READONLY), "unsafe-purge-file");
			identity = std::to_string(info.dwVolumeSerialNumber) + ":" + std::to_string(info.nFileIndexHigh) + ":" + std::to_string(info.nFileIndexLow) + ":" +
				std::to_string(info.ftCreationTime.dwHighDateTime) + ":" + std::to_string(info.ftCreationTime.dwLowDateTime);
			size = uint64_t(info.nFileSizeHigh) << 32 | info.nFileSizeLow;
		}
		std::string Digest()
		{
			Require(size > 0 && size <= s_bodyLimit, "unsafe-purge-file");
			LARGE_INTEGER start{}; Require(SetFilePointerEx(file.value, start, nullptr, FILE_BEGIN), "read-failed");
			SHA256_CTX context{}; SHA256_Init(&context); std::array<uint8_t, 65536> bytes{}; uint64_t read = 0;
			while (read < size)
			{
				DWORD count = 0; Require(ReadFile(file.value, bytes.data(), static_cast<DWORD>(std::min<uint64_t>(bytes.size(), size - read)), &count, nullptr) && count, "read-failed");
				SHA256_Update(&context, bytes.data(), count); read += count;
			}
			std::array<uint8_t, SHA256_DIGEST_LENGTH> digest{}; SHA256_Final(digest.data(), &context);
			constexpr char hex[] = "0123456789abcdef"; std::string result;
			for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; } return result;
		}
		void Remove()
		{
			BY_HANDLE_FILE_INFORMATION info{};
			Require(GetFileInformationByHandle(file.value, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
				(isDirectory || info.nNumberOfLinks == 1), "unsafe-purge-file");
			FILE_DISPOSITION_INFO disposition{TRUE};
			Require(SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition)), "purge-write-failed");
			CloseHandle(file.value); file.value = INVALID_HANDLE_VALUE;
		}
	};
	struct PurgeFile { std::string name, hash, identity; uint64_t size = 0; };
	struct PurgeFolder { std::string root, key, identity, manifest; std::vector<PurgeFile> files; };
	struct PurgeItem { std::string key, name, world, owner; uint32_t account = 0; uint64_t size = 0; std::vector<PurgeFolder> folders; std::string retained; };
	struct PurgePlan { std::string token; std::vector<PurgeItem> items; bool all = false; };
	struct PurgeIssue { std::string key, name, reason; };
	Bytes DecodeImage(const fs::path& path)
	{
		const auto bytes = Read(path, 16 * 1024 * 1024);
		Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
		Require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "invalid-image");
		Microsoft::WRL::ComPtr<IWICStream> stream;
		Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
		Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
		Require(SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) &&
			SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)), "invalid-image");
		GUID container{}; UINT count = 0, width = 0, height = 0;
		Require(SUCCEEDED(decoder->GetContainerFormat(&container)) && (container == GUID_ContainerFormatPng || container == GUID_ContainerFormatJpeg) &&
			SUCCEEDED(decoder->GetFrameCount(&count)) && count == 1 && SUCCEEDED(decoder->GetFrame(0, &frame)) &&
			SUCCEEDED(frame->GetSize(&width, &height)) && width && height && width <= 8192 && height <= 8192 && uint64_t(width) * height <= 32 * 1024 * 1024, "invalid-image");
		Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
		Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
		Require(SUCCEEDED(factory->CreateBitmapScaler(&scaler)) && SUCCEEDED(scaler->Initialize(frame.Get(), 64, 64, WICBitmapInterpolationModeFant)) &&
			SUCCEEDED(factory->CreateFormatConverter(&converter)) && SUCCEEDED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)), "invalid-image");
		Bytes result(64 * 64 * 4);
		Require(SUCCEEDED(converter->CopyPixels(nullptr, 256, static_cast<UINT>(result.size()), result.data())), "invalid-image");
		return result;
	}
}

struct WorldService::Impl
{
	fs::path config, save, marker, backups, trashRoot;
	uint64_t revision = 1, accountRevision = 0;
	uint32_t activeId = 0;
	std::string notice = "ready", previewToken, previewWorld, previewVersion;
	Bytes preview;
	std::unique_ptr<WorldImport> importSource;
	std::string importToken;
	std::unique_ptr<PurgePlan> purgePreview;
	std::vector<PurgeIssue> purgeIssues;
	size_t purgeTotal = 0, purgeCompleted = 0, purgePartial = 0;
	bool healthy = true;
	struct World
	{
		std::string id, name, icon, version, problem;
		uint64_t size = 0;
	};
	std::map<std::string, World> worlds;
	struct TrashData
	{
		std::string key, id, name, icon, version, problem, bodyHash, extHash, restored;
		uint32_t account = 0;
		uint64_t size = 0;
		int64_t deletedAt = 0, bodyTime = 0, extTime = 0;
		bool deleted = false, cancelled = false;
		std::string purgeProblem;
		Bytes body, ext;
	};
	std::map<std::string, TrashData> trash;
	struct Transaction
	{
		std::string operation, world, trash;
		uint32_t source = 0, destination = 0;
		fs::file_time_type bodyTime, extTime;
	};
	Impl(fs::path configRoot, fs::path saveRoot) : config(std::move(configRoot)), save(std::move(saveRoot)), marker(config / ".launcher-world-transaction"), backups(config / ".launcher-world-backups"), trashRoot(config / ".launcher-world-trash")
	{
		CheckPath(config); CheckPath(save); CheckPath(marker); healthy = !fs::exists(marker);
	}
	void Account(const Context& context, uint32_t id)
	{
		Require(id >= 0x80000001 && std::count_if(context.accounts.begin(), context.accounts.end(), [id](const auto& account) { return account.id == id; }) == 1, "invalid-account");
	}
	fs::path Folder(uint32_t id) const { return save / "user" / Hex(id); }
	fs::path Body(uint32_t id, const std::string& world) const { return Folder(id) / world; }
	fs::path Ext(uint32_t id, const std::string& world) const { return Folder(id) / (world + ".ext"); }
	std::string Version(const fs::path& body, std::span<const uint8_t> ext)
	{
		CheckPath(body); Require(fs::is_regular_file(body) && fs::file_size(body) > 0 && fs::file_size(body) <= s_bodyLimit, "missing-body");
		return Hash(ext) + ":" + Hash(Read(body, s_bodyLimit));
	}
	void Scan(const Context& context)
	{
		Account(context, context.activeId);
		if (activeId != context.activeId || accountRevision != context.accountRevision)
		{
			activeId = context.activeId; accountRevision = context.accountRevision; ++revision; preview.clear(); previewToken.clear(); importSource.reset(); importToken.clear(); purgePreview.reset(); purgeIssues.clear();
		}
		worlds.clear(); CheckPath(Folder(activeId));
		if (!fs::exists(Folder(activeId))) return;
		Require(fs::is_directory(Folder(activeId)), "unsafe-path");
		std::set<std::string> ids;
		for (const auto& entry : fs::directory_iterator(Folder(activeId)))
		{
			auto name = entry.path().filename().native();
			if (name.ends_with(L".ext")) name.resize(name.size() - 4);
			if (name.size() == 12 && std::all_of(name.begin(), name.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) ids.insert(std::string(name.begin(), name.end()));
			Require(ids.size() <= 1000, "world-limit");
		}
		for (const auto& id : ids)
		{
			World world; world.id = id;
			try
			{
				const auto ext = Read(Ext(activeId, id), s_extLimit);
				const auto metadata = WorldMetadata::Parse(ext);
				world.name = metadata.name; world.version = Version(Body(activeId, id), ext);
				world.size = fs::file_size(Body(activeId, id)) + ext.size();
				world.icon = "data:image/png;base64," + WorldMetadata::Base64(WorldMetadata::Image(ext));
			}
			catch (...) { world.problem = "unreadable-world"; }
			worlds.emplace(id, std::move(world));
		}
	}
	std::string Snapshot(const Context& context)
	{
		rapidjson::StringBuffer buffer; Writer writer(buffer); writer.StartObject();
		writer.Key("activeId"); writer.Uint(activeId); writer.Key("revision"); writer.Uint64(revision);
		writer.Key("canEdit"); writer.Bool(context.canEdit && healthy); writer.Key("recoveryRequired"); writer.Bool(!healthy);
		Text(writer, "notice", notice); writer.Key("worlds"); writer.StartArray();
		for (const auto& [id, world] : worlds)
		{
			writer.StartObject(); Text(writer, "id", id); Text(writer, "name", world.name); Text(writer, "icon", world.icon);
			Text(writer, "version", world.version); Text(writer, "problem", world.problem); writer.Key("size"); writer.Uint64(world.size); writer.EndObject();
		}
		writer.EndArray(); writer.Key("accounts"); writer.StartArray();
		for (const auto& account : context.accounts)
		{
			if (account.id == activeId) continue;
			writer.StartObject(); writer.Key("id"); writer.Uint(account.id); Text(writer, "name", account.name); writer.EndObject();
		}
		writer.EndArray();
		writer.Key("trash"); writer.StartArray(); uint64_t trashBytes = 0;
		for (const auto& [key, entry] : trash)
		{
			writer.StartObject(); Text(writer, "key", key); Text(writer, "id", entry.id); Text(writer, "name", entry.name); Text(writer, "icon", entry.icon);
			Text(writer, "version", entry.version); Text(writer, "problem", entry.problem); Text(writer, "purgeProblem", entry.purgeProblem); Text(writer, "status", entry.cancelled ? "cancelled" : !entry.restored.empty() ? "restored" : entry.deleted ? "deleted" : "incomplete");
			writer.Key("account"); writer.Uint(entry.account); writer.Key("deletedAt"); writer.Int64(entry.deletedAt); writer.Key("size"); writer.Uint64(entry.size);
			writer.Key("accountAvailable"); writer.Bool(std::any_of(context.accounts.begin(), context.accounts.end(), [&](const auto& account) { return account.id == entry.account; }));
			writer.EndObject(); trashBytes += entry.size;
		}
		writer.EndArray(); writer.Key("trashBytes"); writer.Uint64(trashBytes);
		const bool purgePending = IsPurgePending();
		Text(writer, "recoveryMode", purgePending ? "purge" : "rollback");
		if (purgePending) { try { const auto plan = ReadPurgePlan(ReadText(marker, 128).substr(6)); PurgeProgress(plan); } catch (...) {} }
		writer.Key("purgeProgress"); writer.StartObject(); writer.Key("total"); writer.Uint64(purgeTotal); writer.Key("completed"); writer.Uint64(purgeCompleted);
		writer.Key("remaining"); writer.Uint64(purgeTotal - purgeCompleted); writer.Key("partial"); writer.Uint64(purgePartial); writer.Key("protected"); writer.Uint64(purgeIssues.size()); writer.EndObject();
		writer.Key("purgeIssues"); writer.StartArray();
		for (const auto& issue : purgeIssues) { writer.StartObject(); Text(writer, "key", issue.key); Text(writer, "name", issue.name); Text(writer, "reason", issue.reason); writer.EndObject(); }
		writer.EndArray();
		if (purgePreview)
		{
			writer.Key("purgePreview"); writer.StartObject(); Text(writer, "token", purgePreview->token); writer.Key("count"); writer.Uint64(purgePreview->items.size()); writer.Key("size");
			uint64_t total = 0; for (const auto& item : purgePreview->items) total += item.size; writer.Uint64(total); writer.Key("retainedBackups"); writer.StartArray();
			for (const auto& item : purgePreview->items) if (!item.retained.empty()) { writer.StartObject(); Text(writer, "name", item.name); Text(writer, "reason", item.retained); writer.EndObject(); }
			writer.EndArray(); writer.EndObject();
		}
		if (importSource)
		{
			writer.Key("importPreview"); writer.StartObject(); Text(writer, "token", importToken); writer.Key("candidate"); writer.StartObject();
			const auto& candidate = importSource->Preview(); Text(writer, "id", candidate.id); Text(writer, "account", candidate.account); Text(writer, "name", candidate.name); Text(writer, "icon", candidate.icon);
			writer.Key("size"); writer.Uint64(candidate.size); Text(writer, "worldFile", candidate.worldFile); Text(writer, "metadataFile", candidate.metadataFile); Text(writer, "imageFile", candidate.imageFile); Text(writer, "problem", candidate.problem);
			writer.Key("ready"); writer.Bool(candidate.ready); writer.Key("generated"); writer.Bool(candidate.generated); writer.EndObject(); writer.EndObject();
		}
		if (!preview.empty())
		{
			writer.Key("preview"); writer.StartObject(); Text(writer, "token", previewToken); Text(writer, "world", previewWorld);
			Text(writer, "icon", "data:image/png;base64," + WorldMetadata::Base64(WorldMetadata::Image(preview))); writer.EndObject();
		}
		writer.EndObject(); return buffer.GetString();
	}
	std::string Validate(const rapidjson::Document& json, const Context& context, bool recovery = false)
	{
		Require(context.canEdit, "not-ready"); Require(recovery || healthy, "recovery-required");
		Require(json.HasMember("activeId") && json["activeId"].IsUint() && json["activeId"].GetUint() == context.activeId &&
			json.HasMember("revision") && json["revision"].IsUint64() && json["revision"].GetUint64() == revision, "stale-request");
		Account(context, context.activeId);
		if (recovery) return {};
		Require(json.HasMember("world") && json.HasMember("version"), "invalid-request");
		const auto id = String(json["world"]);
		Require(SaveId(id) && worlds.contains(id) && worlds.at(id).problem.empty(), "invalid-world");
		Require(String(json["version"]) == worlds.at(id).version, "stale-world");
		return id;
	}
	void Verify(const fs::path& path, std::string_view hash, size_t limit)
	{
		Require(Hash(Read(path, limit)) == hash, "verification-failed");
	}
	void RemoveKnown(const fs::path& path, std::string_view hash, size_t limit)
	{
		if (!fs::exists(path)) return;
		Verify(path, hash, limit); CheckPath(path); Require(DeleteFileW(path.c_str()), "write-failed");
	}
	fs::path Backup(Transaction& transaction, std::span<const uint8_t> body, std::span<const uint8_t> ext, std::span<const uint8_t> edited = {})
	{
		CheckPath(backups); fs::create_directories(backups); CheckPath(backups);
		Require(fs::space(config).available >= body.size() + ext.size() + edited.size() + 1024 * 1024, "insufficient-space");
		const fs::path folder = backups / NewGuid(); Require(fs::create_directory(folder), "write-failed");
		if (transaction.operation == "delete") transaction.trash = folder.filename().string();
		WriteNew(folder / "body", body); WriteNew(folder / "metadata", ext);
		if (!edited.empty()) WriteNew(folder / "edited", edited);
		rapidjson::StringBuffer buffer; Writer writer(buffer); writer.StartObject();
		writer.Key("version"); writer.Uint(2); Text(writer, "operation", transaction.operation); Text(writer, "trash", transaction.trash); writer.Key("createdAt"); writer.Int64(NowMilliseconds());
		Text(writer, "world", transaction.world); writer.Key("source"); writer.Uint(transaction.source); writer.Key("destination"); writer.Uint(transaction.destination);
		Text(writer, "bodyHash", Hash(body)); Text(writer, "extHash", Hash(ext)); Text(writer, "editedHash", edited.empty() ? "" : Hash(edited));
		writer.Key("bodySize"); writer.Uint64(body.size()); writer.Key("extSize"); writer.Uint64(ext.size());
		writer.Key("bodyTime"); writer.Int64(transaction.bodyTime.time_since_epoch().count());
		writer.Key("extTime"); writer.Int64(transaction.extTime.time_since_epoch().count());
		writer.EndObject(); WriteText(folder / "manifest.json", buffer.GetString());
		Verify(folder / "body", Hash(body), s_bodyLimit); Verify(folder / "metadata", Hash(ext), s_extLimit);
		if (!edited.empty()) Verify(folder / "edited", Hash(edited), s_extLimit);
		healthy = false; WriteText(marker, folder.filename().string());
		return folder;
	}
	void ClearMarker()
	{
		CheckPath(marker); Require(DeleteFileW(marker.c_str()), "write-failed"); healthy = true;
	}
	TrashData LoadTrash(const std::string& key, bool verifyBody = true)
	{
		Require(Guid(key), "invalid-trash"); const auto folder = trashRoot / key; DirectoryGuard guard(folder);
		const auto bytes = Read(folder / "manifest.json", 8192); const auto json = Json({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
		Require(json.MemberCount() == 12 && json.HasMember("version") && json["version"].IsUint() && json["version"].GetUint() == 2 &&
			json.HasMember("world") && json.HasMember("account") && json["account"].IsUint() && json.HasMember("deletedAt") && json["deletedAt"].IsInt64() &&
			json.HasMember("bodyHash") && json.HasMember("extHash") && json.HasMember("bodySize") && json["bodySize"].IsUint64() && json.HasMember("extSize") && json["extSize"].IsUint64() &&
			json.HasMember("bodyTime") && json["bodyTime"].IsInt64() && json.HasMember("extTime") && json["extTime"].IsInt64() && json.HasMember("originalPath") && json.HasMember("transaction"), "invalid-trash");
		TrashData entry; entry.key = key; entry.id = String(json["world"]); entry.account = json["account"].GetUint(); entry.deletedAt = json["deletedAt"].GetInt64();
		Require(SaveId(entry.id) && entry.account >= 0x80000001 && String(json["transaction"]) == key && String(json["originalPath"]) == "user/" + Hex(entry.account) + "/" + entry.id, "invalid-trash");
		entry.bodyHash = String(json["bodyHash"]); entry.extHash = String(json["extHash"]); entry.bodyTime = json["bodyTime"].GetInt64(); entry.extTime = json["extTime"].GetInt64();
		CheckPath(folder / "body"); const auto bodySize = fs::file_size(folder / "body"); Require(bodySize > 0 && bodySize <= s_bodyLimit && bodySize == json["bodySize"].GetUint64(), "invalid-trash");
		if (verifyBody) { entry.body = Read(folder / "body", s_bodyLimit); Require(entry.body.size() == bodySize && Hash(entry.body) == entry.bodyHash, "invalid-trash"); }
		entry.ext = Read(folder / "metadata", s_extLimit);
		Require(entry.ext.size() == json["extSize"].GetUint64() && Hash(entry.ext) == entry.extHash, "invalid-trash");
		const auto metadata = WorldMetadata::Parse(entry.ext); entry.name = metadata.name; entry.size = bodySize + entry.ext.size();
		entry.icon = "data:image/png;base64," + WorldMetadata::Base64(WorldMetadata::Image(entry.ext));
		auto receipt = [&](const char* name) {
			if (!fs::exists(folder / name)) return std::string();
			const auto value = Read(folder / name, 128); return std::string(value.begin(), value.end());
		};
		const auto deleted = receipt("deleted"), cancelled = receipt("cancelled"); entry.restored = receipt("restored");
		Require((deleted.empty() || deleted == key) && (cancelled.empty() || cancelled == key) && (entry.restored.empty() || Guid(entry.restored)), "invalid-trash");
		entry.deleted = !deleted.empty(); entry.cancelled = !cancelled.empty(); entry.version = Hash(bytes) + ":" + entry.bodyHash + ":" + entry.extHash + ":" + deleted + cancelled + entry.restored;
		return entry;
	}
	void ScanTrash()
	{
		trash.clear(); CheckPath(trashRoot); if (!fs::exists(trashRoot)) return; DirectoryGuard guard(trashRoot);
		size_t count = 0, iconBytes = 0;
		for (const auto& folder : fs::directory_iterator(trashRoot))
		{
			Require(++count <= 4096, "trash-count-limit"); TrashData entry;
			try
			{
				const auto key = folder.path().filename().string(); entry.key = Guid(key) ? key : "invalid-" + std::to_string(count);
				entry = LoadTrash(key, false);
				try { PurgeOwnership(entry); } catch (const std::exception& error) { entry.purgeProblem = PurgeReason(error); }
				entry.ext.clear();
				if (entry.icon.size() > 16 * 1024 * 1024 - iconBytes) entry.icon.clear(); else iconBytes += entry.icon.size();
			}
			catch (...) { if (entry.key.empty()) entry.key = "invalid-" + std::to_string(count); entry.problem = "unreadable-trash"; }
			trash.emplace(entry.key, std::move(entry));
		}
	}
	fs::path PurgeRoot() const { return config / ".launcher-world-purges"; }
	fs::path PurgeFolderPath(const PurgeFolder& folder) const
	{
		Require((folder.root == "trash" || folder.root == "backup") && Guid(folder.key), "invalid-purge");
		return (folder.root == "trash" ? trashRoot : backups) / folder.key;
	}
	std::string ReadText(const fs::path& path, size_t limit = 8192)
	{
		const auto bytes = Read(path, limit); return {bytes.begin(), bytes.end()};
	}
	void CheckPurgeLink(const TrashData& entry, const rapidjson::Value& document, const std::string& operation)
	{
		Keys(document, {"version","operation","trash","createdAt","world","source","destination","bodyHash","extHash","editedHash","bodySize","extSize","bodyTime","extTime"});
		Require(document["version"].IsUint() && document["version"].GetUint() == 2 && document["source"].IsUint() && document["destination"].IsUint() &&
			document["bodySize"].IsUint64() && document["extSize"].IsUint64() && document["bodyTime"].IsInt64() && document["extTime"].IsInt64() && document["createdAt"].IsInt64(), "purge-owner-mismatch");
		Require(String(document["operation"]) == operation && String(document["trash"]) == entry.key && String(document["world"]) == entry.id &&
			String(document["bodyHash"]) == entry.bodyHash && String(document["extHash"]) == entry.extHash && String(document["editedHash"]).empty() &&
			document["bodySize"].GetUint64() + document["extSize"].GetUint64() == entry.size && document["bodyTime"].GetInt64() == entry.bodyTime && document["extTime"].GetInt64() == entry.extTime &&
			document["source"].GetUint() == (operation == "delete" ? entry.account : 0) && document["destination"].GetUint() == (operation == "delete" ? 0 : entry.account), "purge-owner-mismatch");
	}
	std::set<std::string> PurgeNames(const std::string& root) const
	{
		if (root == "trash") return {"body","metadata","manifest.json","deleted","cancelled","restored"};
		return {"body","metadata","manifest.json","trash-created","trash-verified","destination-verified","completed"};
	}
	void CheckPurgeNames(const fs::path& folder, const std::set<std::string>& allowed)
	{
		for (const auto& file : fs::directory_iterator(folder))
		{
			Require(allowed.contains(file.path().filename().string()), "purge-unknown-file");
			PurgeHandle checked(file.path(), false, false);
		}
	}
	std::string PurgeOwnership(const TrashData& entry)
	{
		Require(entry.deleted || entry.cancelled, "purge-incomplete");
		Require(!(entry.cancelled && !entry.restored.empty()), "purge-owner-mismatch");
		CheckPurgeNames(trashRoot / entry.key, PurgeNames("trash"));
		const auto owner = ReadText(backups / entry.key / "manifest.json"); CheckPurgeLink(entry, Json(owner), "delete");
		Require(ReadText(backups / entry.key / "trash-created", 128) == entry.key && ReadText(backups / entry.key / "trash-verified", 128) == entry.key, "purge-owner-mismatch");
		if (!entry.restored.empty())
		{
			CheckPurgeLink(entry, Json(ReadText(backups / entry.restored / "manifest.json")), "restore");
			Require(ReadText(backups / entry.restored / "completed", 128) == "committed" && ReadText(backups / entry.restored / "destination-verified", 128) == "verified", "purge-owner-mismatch");
		}
		return owner;
	}
	PurgeFolder CapturePurgeFolder(const std::string& root, const std::string& key, const TrashData& entry)
	{
		PurgeFolder folder; folder.root = root; folder.key = key; const auto path = PurgeFolderPath(folder); DirectoryGuard guard(path);
		PurgeHandle directory(path, true, false); folder.identity = directory.identity;
		CheckPurgeNames(path, PurgeNames(root)); folder.manifest = ReadText(path / "manifest.json");
		if (root == "backup") CheckPurgeLink(entry, Json(folder.manifest), key == entry.key ? "delete" : "restore");
		for (const auto& file : fs::directory_iterator(path))
		{
			PurgeHandle checked(file.path(), false, false); const auto name = file.path().filename().string();
			const auto hash = checked.Digest();
			if (name == "body") Require(hash == entry.bodyHash, "verification-failed");
			if (name == "metadata") Require(hash == entry.extHash, "verification-failed");
			if (name == "completed") Require(ReadText(file.path(), 128) == "committed", "purge-owner-mismatch");
			folder.files.push_back({name, hash, checked.identity, checked.size});
		}
		return folder;
	}
	PurgeItem PreparePurgeItem(const std::string& key, const std::map<std::string, std::string>& references, bool unknownReferences)
	{
		const auto entry = LoadTrash(key); PurgeItem item;
		item.key = key; item.name = entry.name; item.world = entry.id; item.account = entry.account; item.size = entry.size; item.owner = PurgeOwnership(entry);
		item.folders.push_back(CapturePurgeFolder("trash", key, entry));
		bool shared = unknownReferences;
		for (const auto& [transaction, referenced] : references)
			if (referenced == key && transaction != key && transaction != entry.restored) shared = true;
		if (shared) item.retained = "purge-backup-referenced";
		else
		{
			try
			{
				std::vector<PurgeFolder> related; related.push_back(CapturePurgeFolder("backup", key, entry));
				if (!entry.restored.empty()) related.push_back(CapturePurgeFolder("backup", entry.restored, entry));
				for (auto& folder : related) item.folders.push_back(std::move(folder));
			}
			catch (...) { item.retained = "purge-backup-protected"; }
		}
		return item;
	}
	std::string PurgeReason(const std::exception& error) const
	{
		const std::set<std::string> reasons{"purge-incomplete","purge-owner-mismatch","purge-unknown-file","unsafe-purge-file","purge-file-busy","verification-failed","invalid-trash","invalid-purge","read-failed","unsafe-path","invalid-file","purge-write-failed","purge-changed","purge-backup-referenced"};
		return reasons.contains(error.what()) ? error.what() : "purge-protected";
	}
	std::string BackupReference(const rapidjson::Value& document)
	{
		if (document.HasMember("version"))
		{
			Keys(document, {"version","operation","trash","createdAt","world","source","destination","bodyHash","extHash","editedHash","bodySize","extSize","bodyTime","extTime"});
			const std::set<std::string> operations{"rename","icon","move","delete","import","restore"};
			Require(document["version"].IsUint() && document["version"].GetUint() == 2 && document["createdAt"].IsInt64() && operations.contains(String(document["operation"])), "purge-protected");
			const auto reference = String(document["trash"]); Require(reference.empty() || Guid(reference), "purge-protected");
		}
		else Keys(document, {"world","source","destination","bodyHash","extHash","editedHash","bodySize","extSize","bodyTime","extTime"});
		Require(SaveId(String(document["world"])) && document["source"].IsUint() && document["destination"].IsUint() && document["bodySize"].IsUint64() && document["extSize"].IsUint64() && document["bodyTime"].IsInt64() && document["extTime"].IsInt64(), "purge-protected");
		for (const auto key : {"bodyHash","extHash","editedHash"})
		{
			const auto hash = String(document[key]);
			Require((hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) || (std::string_view(key) == "editedHash" && hash.empty()), "purge-protected");
		}
		return document.HasMember("trash") ? String(document["trash"]) : "";
	}
	void PreparePurge(const rapidjson::Document& request)
	{
		purgePreview.reset(); purgeIssues.clear(); ScanTrash();
		const bool single = request.HasMember("trash");
		Require(request.MemberCount() == (single ? 5 : 3) && (!single || request.HasMember("version")), "invalid-request");
		const auto key = single ? String(request["trash"]) : "";
		if (single) Require(trash.contains(key) && trash.at(key).version == String(request["version"]), "stale-trash");
		std::map<std::string, std::string> references; bool unknown = false; size_t count = 0;
		if (fs::exists(backups)) for (const auto& folder : fs::directory_iterator(backups))
		{
			Require(++count <= 16384, "purge-backup-limit");
			try
			{
				const auto document = Json(ReadText(folder.path() / "manifest.json"));
				references.emplace(folder.path().filename().string(), BackupReference(document));
			}
			catch (...) { unknown = true; }
		}
		PurgePlan plan; plan.token = NewGuid(); plan.all = !single;
		for (const auto& [candidate, entry] : trash)
		{
			if (single && candidate != key) continue;
			try { plan.items.push_back(PreparePurgeItem(candidate, references, unknown)); }
			catch (const std::exception& error) { purgeIssues.push_back({candidate, entry.name, PurgeReason(error)}); }
		}
		Require(!plan.items.empty() || !purgeIssues.empty(), "trash-empty");
		if (purgeIssues.empty()) { Require(PurgeJson(plan).size() <= 16 * 1024 * 1024, "purge-plan-limit"); purgePreview = std::make_unique<PurgePlan>(std::move(plan)); notice = "purge-preview-ready"; }
		else notice = "purge-blocked";
	}
	std::string PurgeJson(const PurgePlan& plan)
	{
		rapidjson::StringBuffer buffer; Writer writer(buffer); writer.StartObject(); writer.Key("version"); writer.Uint(1); Text(writer, "operation", "purge"); writer.Key("items"); writer.StartArray();
		for (const auto& item : plan.items)
		{
			writer.StartObject(); Text(writer, "key", item.key); Text(writer, "name", item.name); Text(writer, "world", item.world); Text(writer, "owner", item.owner);
			writer.Key("account"); writer.Uint(item.account); writer.Key("size"); writer.Uint64(item.size); writer.Key("folders"); writer.StartArray();
			for (const auto& folder : item.folders)
			{
				writer.StartObject(); Text(writer, "root", folder.root); Text(writer, "key", folder.key); Text(writer, "identity", folder.identity); Text(writer, "manifest", folder.manifest); writer.Key("files"); writer.StartArray();
				for (const auto& file : folder.files)
				{
					writer.StartObject(); Text(writer, "name", file.name); Text(writer, "hash", file.hash); Text(writer, "identity", file.identity); writer.Key("size"); writer.Uint64(file.size); writer.EndObject();
				}
				writer.EndArray(); writer.EndObject();
			}
			writer.EndArray(); writer.EndObject();
		}
		writer.EndArray(); writer.EndObject(); return buffer.GetString();
	}
	PurgePlan ReadPurgePlan(const std::string& id)
	{
		Require(Guid(id), "invalid-purge"); const auto document = Json(ReadText(PurgeRoot() / id / "manifest.json", 16 * 1024 * 1024), 16 * 1024 * 1024);
		Keys(document, {"version","operation","items"}); Require(document["version"].IsUint() && document["version"].GetUint() == 1 && String(document["operation"]) == "purge" && document["items"].IsArray(), "invalid-purge");
		Require(!document["items"].Empty() && document["items"].Size() <= 4096, "invalid-purge"); PurgePlan plan; plan.token = id; std::set<fs::path> unique;
		for (const auto& value : document["items"].GetArray())
		{
			Keys(value, {"key","name","world","owner","account","size","folders"}); PurgeItem item;
			item.key = String(value["key"]); item.name = String(value["name"]); item.world = String(value["world"]); item.owner = String(value["owner"]);
			Require(Guid(item.key) && SaveId(item.world) && value["account"].IsUint() && value["account"].GetUint() >= 0x80000001 && value["size"].IsUint64(), "invalid-purge");
			item.account = value["account"].GetUint(); item.size = value["size"].GetUint64();
			Require(value["folders"].IsArray() && value["folders"].Size() >= 1 && value["folders"].Size() <= 3, "invalid-purge");
			TrashData entry; entry.key = item.key; entry.id = item.world; entry.account = item.account; entry.size = item.size;
			const auto owner = Json(item.owner); Require(owner.HasMember("bodyHash") && owner.HasMember("extHash") && owner.HasMember("bodyTime") && owner["bodyTime"].IsInt64() && owner.HasMember("extTime") && owner["extTime"].IsInt64(), "invalid-purge");
			entry.bodyHash = String(owner["bodyHash"]); entry.extHash = String(owner["extHash"]); entry.bodyTime = owner["bodyTime"].GetInt64(); entry.extTime = owner["extTime"].GetInt64(); CheckPurgeLink(entry, owner, "delete");
			bool first = true;
			for (const auto& node : value["folders"].GetArray())
			{
				Keys(node, {"root","key","identity","manifest","files"}); PurgeFolder folder;
				folder.root = String(node["root"]); folder.key = String(node["key"]); folder.identity = String(node["identity"]); folder.manifest = String(node["manifest"]);
				Require(unique.insert(PurgeFolderPath(folder)).second && ((first && folder.root == "trash" && folder.key == item.key) || (!first && folder.root == "backup")), "invalid-purge");
				const auto manifest = Json(folder.manifest);
				if (first)
				{
					Keys(manifest, {"version","world","account","deletedAt","bodyHash","extHash","bodySize","extSize","bodyTime","extTime","originalPath","transaction"});
					Require(manifest["version"].IsUint() && manifest["version"].GetUint() == 2 && manifest["account"].IsUint() && manifest["account"].GetUint() == entry.account &&
						manifest["bodySize"].IsUint64() && manifest["extSize"].IsUint64() && manifest["bodySize"].GetUint64() == owner["bodySize"].GetUint64() && manifest["extSize"].GetUint64() == owner["extSize"].GetUint64() &&
						manifest["bodyTime"].IsInt64() && manifest["extTime"].IsInt64() && manifest["bodyTime"].GetInt64() == entry.bodyTime && manifest["extTime"].GetInt64() == entry.extTime && manifest["deletedAt"].IsInt64() &&
						String(manifest["originalPath"]) == "user/" + Hex(entry.account) + "/" + entry.id && String(manifest["world"]) == entry.id && String(manifest["transaction"]) == entry.key && String(manifest["bodyHash"]) == entry.bodyHash && String(manifest["extHash"]) == entry.extHash, "invalid-purge");
				}
				else CheckPurgeLink(entry, manifest, folder.key == item.key ? "delete" : "restore");
				Require(node["files"].IsArray() && node["files"].Size() >= 3 && node["files"].Size() <= 7, "invalid-purge"); std::set<std::string> names;
				for (const auto& data : node["files"].GetArray())
				{
					Keys(data, {"name","hash","identity","size"}); PurgeFile file{String(data["name"]),String(data["hash"]),String(data["identity"])};
					Require(PurgeNames(folder.root).contains(file.name) && names.insert(file.name).second && data["size"].IsUint64(), "invalid-purge"); file.size = data["size"].GetUint64();
					Require(file.size > 0 && file.size <= s_bodyLimit && file.hash.size() == 64 && !file.identity.empty(), "invalid-purge");
					if (file.name == "body") Require(file.hash == entry.bodyHash, "invalid-purge");
					if (file.name == "metadata") Require(file.hash == entry.extHash, "invalid-purge");
					if (file.name == "manifest.json") Require(file.hash == Hash({reinterpret_cast<const uint8_t*>(folder.manifest.data()), folder.manifest.size()}), "invalid-purge");
					folder.files.push_back(std::move(file));
				}
				Require(names.contains("body") && names.contains("metadata") && names.contains("manifest.json"), "invalid-purge");
				if (first) Require((names.contains("deleted") || names.contains("cancelled")) && (!names.contains("restored") || (names.contains("deleted") && !names.contains("cancelled"))), "invalid-purge");
				item.folders.push_back(std::move(folder)); first = false;
			}
			plan.items.push_back(std::move(item));
		}
		return plan;
	}
	void PurgeProgress(const PurgePlan& plan)
	{
		purgeTotal = plan.items.size(); purgeCompleted = purgePartial = 0;
		for (const auto& item : plan.items)
		{
			bool absent = true, missing = false;
			for (const auto& folder : item.folders)
			{
				const auto path = PurgeFolderPath(folder); if (fs::exists(path)) absent = false; else missing = true;
				for (const auto& file : folder.files) if (!fs::exists(path / file.name)) missing = true;
			}
			if (absent) ++purgeCompleted; else if (missing) ++purgePartial;
		}
	}
	bool IsPurgePending()
	{
		try { return !healthy && fs::exists(marker) && ReadText(marker, 128).starts_with("purge:"); } catch (...) { return false; }
	}
	void CheckPurgeReferences(const PurgePlan& plan)
	{
		std::set<std::string> selected, removing;
		for (const auto& item : plan.items) for (const auto& folder : item.folders)
			if (folder.root == "backup") { selected.insert(folder.key); removing.insert(item.key); }
		if (selected.empty()) return;
		for (const auto& folder : fs::directory_iterator(backups))
		{
			if (selected.contains(folder.path().filename().string())) continue;
			try
			{
				const auto document = Json(ReadText(folder.path() / "manifest.json"));
				Require(!removing.contains(BackupReference(document)), "purge-changed");
			}
			catch (...) { throw std::runtime_error("purge-changed"); }
		}
	}
	void ExecutePurge(const PurgePlan& plan, bool starting)
	{
		DirectoryGuard trashGuard(trashRoot), backupGuard(backups), journalGuard(PurgeRoot());
		if (starting && plan.all)
		{
			std::set<std::string> expected; for (const auto& item : plan.items) expected.insert(item.key);
			for (const auto& folder : fs::directory_iterator(trashRoot)) Require(expected.erase(folder.path().filename().string()) == 1, "purge-changed");
			Require(expected.empty(), "purge-changed");
		}
		CheckPurgeReferences(plan);
		struct LockedFolder { std::unique_ptr<PurgeHandle> directory; std::vector<std::unique_ptr<PurgeHandle>> files; };
		std::vector<LockedFolder> locked; const auto journal = PurgeRoot() / plan.token;
		const auto serialized = PurgeJson(plan), signature = Hash({reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size()});
		Require(serialized.size() <= 16 * 1024 * 1024, "purge-plan-limit");
		const bool started = !starting && fs::exists(journal / "started");
		if (started) Require(ReadText(journal / "started", 128) == signature, "invalid-purge");
		if (!starting)
		{
			for (const auto& item : plan.items)
				if (fs::exists(journal / ("done-" + item.key))) Require(ReadText(journal / ("done-" + item.key), 128) == signature, "invalid-purge");
			if (fs::exists(journal / "completed")) Require(ReadText(journal / "completed", 128) == signature, "invalid-purge");
		}
		purgeIssues.clear();
		std::string currentKey, currentName;
		try
		{
			// 全対象のハンドルを保持してから開始する。再開時も残存ファイルをすべて先に検証する。
			for (const auto& item : plan.items) for (const auto& folder : item.folders)
			{
				currentKey = item.key; currentName = item.name;
				LockedFolder handles; const auto path = PurgeFolderPath(folder);
				if (fs::exists(path))
				{
					handles.directory = std::make_unique<PurgeHandle>(path, true, true); Require(handles.directory->identity == folder.identity, "purge-changed");
					std::set<std::string> names; for (const auto& file : folder.files) names.insert(file.name);
					for (const auto& found : fs::directory_iterator(path)) Require(names.contains(found.path().filename().string()), "purge-unknown-file");
					for (const auto& file : folder.files)
					{
						std::unique_ptr<PurgeHandle> handle;
						if (fs::exists(path / file.name))
						{
							handle = std::make_unique<PurgeHandle>(path / file.name, false, true);
							Require(handle->identity == file.identity && handle->size == file.size && handle->Digest() == file.hash, "purge-changed");
						}
						else Require(started, "purge-changed");
						handles.files.push_back(std::move(handle));
					}
				}
				else Require(started, "purge-changed");
				locked.push_back(std::move(handles));
			}
			if (starting)
			{
				currentKey.clear(); currentName.clear();
				CheckPath(PurgeRoot()); fs::create_directories(PurgeRoot()); Require(fs::create_directory(journal), "write-failed");
				WriteText(journal / "manifest.json", serialized); WriteText(marker, "purge:" + plan.token); healthy = false;
			}
			Receipt(journal / "started", signature);
			size_t index = 0; purgeTotal = plan.items.size(); purgeCompleted = purgePartial = 0;
			for (const auto& item : plan.items)
			{
				currentKey = item.key; currentName = item.name;
				for (const auto& folder : item.folders)
				{
					auto& handles = locked[index++];
					for (auto& file : handles.files) if (file) file->Remove();
					if (handles.directory) handles.directory->Remove();
				}
				Receipt(journal / ("done-" + item.key), signature); ++purgeCompleted;
			}
			Receipt(journal / "completed", signature); ClearMarker(); ++revision; notice = "purged"; purgePreview.reset(); ScanTrash();
		}
		catch (const std::exception& error)
		{
			PurgeProgress(plan); purgeIssues.push_back({currentKey, currentName, PurgeReason(error)});
			// 不可逆な削除の後は元に戻さない。永続化済みの対象だけを明示操作で前進復旧する。
			throw;
		}
	}

	void CreateTrash(const Transaction& transaction, const fs::path& backup, const Bytes& body, const Bytes& ext)
	{
		CheckPath(trashRoot); fs::create_directories(trashRoot); DirectoryGuard guard(trashRoot);
		Require(fs::space(trashRoot).available >= body.size() + ext.size() + 1024 * 1024, "insufficient-space");
		const auto folder = trashRoot / transaction.trash; Require(fs::create_directory(folder), "write-failed"); DirectoryGuard folderGuard(folder);
		Receipt(backup / "trash-created", transaction.trash);
		WriteNew(folder / "body", body); WriteNew(folder / "metadata", ext);
		rapidjson::StringBuffer buffer; Writer writer(buffer); writer.StartObject();
		writer.Key("version"); writer.Uint(2); Text(writer, "world", transaction.world); writer.Key("account"); writer.Uint(transaction.source);
		writer.Key("deletedAt"); writer.Int64(NowMilliseconds()); Text(writer, "bodyHash", Hash(body)); Text(writer, "extHash", Hash(ext));
		writer.Key("bodySize"); writer.Uint64(body.size()); writer.Key("extSize"); writer.Uint64(ext.size());
		writer.Key("bodyTime"); writer.Int64(transaction.bodyTime.time_since_epoch().count()); writer.Key("extTime"); writer.Int64(transaction.extTime.time_since_epoch().count());
		Text(writer, "originalPath", "user/" + Hex(transaction.source) + "/" + transaction.world); Text(writer, "transaction", transaction.trash); writer.EndObject();
		WriteText(folder / "manifest.json", buffer.GetString());
		fs::last_write_time(folder / "body", transaction.bodyTime); fs::last_write_time(folder / "metadata", transaction.extTime);
		const auto entry = LoadTrash(transaction.trash); Require(entry.bodyHash == Hash(body) && entry.extHash == Hash(ext), "verification-failed");
		Receipt(backup / "trash-verified", transaction.trash);
	}
	void CopyToAccount(const Transaction& transaction, const fs::path& backup, const Bytes& body, const Bytes& ext)
	{
		const auto destination = transaction.destination; const auto& id = transaction.world;
		CheckPath(Folder(destination)); fs::create_directories(Folder(destination)); DirectoryGuard guard(Folder(destination));
		Require(!fs::exists(Body(destination, id)) && !fs::exists(Ext(destination, id)), "save-id-conflict");
		Require(fs::space(Folder(destination)).available >= body.size() + ext.size() + 1024 * 1024, "insufficient-space");
		const auto prefix = ".launcher-world-" + backup.filename().string();
		const auto temporaryBody = Folder(destination) / (prefix + ".body.tmp"), temporaryExt = Folder(destination) / (prefix + ".ext.tmp");
		WriteNew(temporaryBody, body); WriteNew(temporaryExt, ext); Verify(temporaryBody, Hash(body), s_bodyLimit); Verify(temporaryExt, Hash(ext), s_extLimit);
		Require(MoveFileExW(temporaryBody.c_str(), Body(destination, id).c_str(), MOVEFILE_WRITE_THROUGH), "write-failed");
		Require(MoveFileExW(temporaryExt.c_str(), Ext(destination, id).c_str(), MOVEFILE_WRITE_THROUGH), "write-failed");
		Verify(Body(destination, id), Hash(body), s_bodyLimit); Verify(Ext(destination, id), Hash(ext), s_extLimit); WorldMetadata::Parse(Read(Ext(destination, id), s_extLimit));
		fs::last_write_time(Body(destination, id), transaction.bodyTime); fs::last_write_time(Ext(destination, id), transaction.extTime);
		Receipt(backup / "destination-verified", "verified");
	}
	void RecoverManaged(const rapidjson::Document& json, const fs::path& backup, const std::string& transactionId, const Context& context)
	{
		const auto operation = String(json["operation"]), world = String(json["world"]), trashId = String(json["trash"]);
		const auto source = json["source"].GetUint(), destination = json["destination"].GetUint();
		const auto bodyHash = String(json["bodyHash"]), extHash = String(json["extHash"]);
		Require(String(json["editedHash"]).empty(), "recovery-required");
		const auto body = Read(backup / "body", s_bodyLimit), ext = Read(backup / "metadata", s_extLimit);
		Require(Hash(body) == bodyHash && Hash(ext) == extHash && body.size() == json["bodySize"].GetUint64() && ext.size() == json["extSize"].GetUint64(), "recovery-required"); WorldMetadata::Parse(ext);
		if (operation == "delete")
		{
			Require(source != 0 && destination == 0 && trashId == transactionId, "recovery-required"); Account(context, source); DirectoryGuard guard(Folder(source));
			const bool missingBody = !fs::exists(Body(source, world)), missingExt = !fs::exists(Ext(source, world));
			if (!missingBody) Verify(Body(source, world), bodyHash, s_bodyLimit);
			if (!missingExt) Verify(Ext(source, world), extHash, s_extLimit);
			const bool verifiedTrash = fs::exists(backup / "trash-verified");
			Require(verifiedTrash || (!missingBody && !missingExt), "recovery-required");
			if (verifiedTrash)
			{
				Receipt(backup / "trash-verified", trashId); const auto entry = LoadTrash(trashId);
				Require(entry.id == world && entry.account == source && entry.bodyHash == bodyHash && entry.extHash == extHash && entry.restored.empty(), "recovery-required");
			}
			if (missingBody) WriteNew(Body(source, world), body);
			if (missingExt) WriteNew(Ext(source, world), ext);
			if (missingBody) fs::last_write_time(Body(source, world), fs::file_time_type(fs::file_time_type::duration(json["bodyTime"].GetInt64())));
			if (missingExt) fs::last_write_time(Ext(source, world), fs::file_time_type(fs::file_time_type::duration(json["extTime"].GetInt64())));
			Verify(Body(source, world), bodyHash, s_bodyLimit); Verify(Ext(source, world), extHash, s_extLimit);
			if (verifiedTrash) Receipt(trashRoot / trashId / "cancelled", trashId);
		}
		else
		{
			Require(source == 0 && destination != 0, "recovery-required"); Account(context, destination); DirectoryGuard guard(Folder(destination));
			if (operation == "restore")
			{
				Require(Guid(trashId), "recovery-required"); const auto entry = LoadTrash(trashId);
				Require(entry.id == world && entry.account == destination && entry.bodyHash == bodyHash && entry.extHash == extHash && entry.deleted && !entry.cancelled && (entry.restored.empty() || entry.restored == transactionId), "recovery-required");
			}
			else Require(operation == "import" && trashId.empty(), "recovery-required");
			// 全ファイルを先に確認し、不明な内容を一つでも見つけたら何も消さない。
			if (fs::exists(Body(destination, world))) Verify(Body(destination, world), bodyHash, s_bodyLimit);
			if (fs::exists(Ext(destination, world))) Verify(Ext(destination, world), extHash, s_extLimit);
			RemoveKnown(Body(destination, world), bodyHash, s_bodyLimit); RemoveKnown(Ext(destination, world), extHash, s_extLimit);
			if (operation == "restore" && fs::exists(trashRoot / trashId / "restored"))
			{
				const auto value = Read(trashRoot / trashId / "restored", 128); Require(std::string(value.begin(), value.end()) == transactionId, "recovery-required");
				Require(DeleteFileW((trashRoot / trashId / "restored").c_str()), "write-failed");
			}
		}
		ClearMarker(); ++revision; notice = "recovered";
	}
	void Recover(const Context& context)
	{
		Require(!healthy && fs::exists(marker), "recovery-required");
		const auto markerBytes = Read(marker, 128);
		const std::string id(markerBytes.begin(), markerBytes.end());
		if (id.starts_with("purge:")) { ExecutePurge(ReadPurgePlan(id.substr(6)), false); return; }
		Require(id.size() == 38 && id.front() == '{' && id.back() == '}' && std::all_of(id.begin() + 1, id.end() - 1, [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') || c == '-'; }), "recovery-required");
		const auto folder = backups / id; const auto manifestBytes = Read(folder / "manifest.json", 8192);
		const auto json = Json({reinterpret_cast<const char*>(manifestBytes.data()), manifestBytes.size()});
		const bool version2 = json.HasMember("version");
		if (version2) Require(json["version"].IsUint() && json["version"].GetUint() == 2 && json.MemberCount() == 14 && json.HasMember("operation") && json.HasMember("trash") && json.HasMember("createdAt") && json["createdAt"].IsInt64(), "recovery-required");
		Require((version2 || json.MemberCount() == 10) && json.HasMember("world") && json.HasMember("source") && json["source"].IsUint() && json.HasMember("destination") && json["destination"].IsUint() && json.HasMember("bodyHash") && json.HasMember("extHash") && json.HasMember("editedHash") &&
			json.HasMember("bodySize") && json["bodySize"].IsUint64() && json.HasMember("extSize") && json["extSize"].IsUint64() &&
			json.HasMember("bodyTime") && json["bodyTime"].IsInt64() && json.HasMember("extTime") && json["extTime"].IsInt64(), "recovery-required");
		const auto world = String(json["world"]); Require(SaveId(world), "recovery-required");
		const auto source = json["source"].GetUint(), destination = json["destination"].GetUint();
		if (version2)
		{
			const auto operation = String(json["operation"]);
			if (operation == "delete" || operation == "import" || operation == "restore") { RecoverManaged(json, folder, id, context); return; }
			Require(String(json["trash"]).empty() && ((operation == "move" && destination != 0 && String(json["editedHash"]).empty()) || ((operation == "rename" || operation == "icon") && destination == 0 && !String(json["editedHash"]).empty())), "recovery-required");
		}
		Account(context, source); if (destination) { Account(context, destination); Require(source != destination, "recovery-required"); }
		const auto bodyHash = String(json["bodyHash"]), extHash = String(json["extHash"]), editedHash = String(json["editedHash"]);
		const auto body = Read(folder / "body", s_bodyLimit), ext = Read(folder / "metadata", s_extLimit);
		Require(Hash(body) == bodyHash && Hash(ext) == extHash && body.size() == json["bodySize"].GetUint64() && ext.size() == json["extSize"].GetUint64(), "recovery-required"); WorldMetadata::Parse(ext);
		DirectoryGuard sourceGuard(Folder(source)), destinationGuard(destination ? Folder(destination) : Folder(source));
		// 全対象を先に照合し、不明な状態では一切変更しない。
		if (fs::exists(Body(source, world))) Verify(Body(source, world), bodyHash, s_bodyLimit);
		if (fs::exists(Ext(source, world)))
		{
			const auto current = Hash(Read(Ext(source, world), s_extLimit));
			Require(current == extHash || (!destination && current == editedHash), "recovery-required");
		}
		if (destination)
		{
			if (fs::exists(Body(destination, world))) Verify(Body(destination, world), bodyHash, s_bodyLimit);
			if (fs::exists(Ext(destination, world))) Verify(Ext(destination, world), extHash, s_extLimit);
		}
		else Require(fs::exists(Body(source, world)) && fs::exists(Ext(source, world)), "recovery-required");
		const bool restoreBody = !fs::exists(Body(source, world));
		bool restoreExt = !fs::exists(Ext(source, world));
		if (restoreBody) WriteNew(Body(source, world), body);
		if (restoreExt) WriteNew(Ext(source, world), ext);
		else if (!destination && Hash(Read(Ext(source, world), s_extLimit)) == editedHash)
		{
			const auto temporary = Folder(source) / (".launcher-world-" + id + ".restore"); WriteNew(temporary, ext);
			Require(ReplaceFileW(Ext(source, world).c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr), "recovery-required");
			restoreExt = true;
		}
		if (restoreBody) fs::last_write_time(Body(source, world), fs::file_time_type(fs::file_time_type::duration(json["bodyTime"].GetInt64())));
		if (restoreExt) fs::last_write_time(Ext(source, world), fs::file_time_type(fs::file_time_type::duration(json["extTime"].GetInt64())));
		Verify(Body(source, world), bodyHash, s_bodyLimit); Verify(Ext(source, world), extHash, s_extLimit);
		if (destination)
		{
			RemoveKnown(Body(destination, world), bodyHash, s_bodyLimit); RemoveKnown(Ext(destination, world), extHash, s_extLimit);
		}
		ClearMarker(); ++revision; notice = "recovered";
	}
	void Commit(const std::string& id, uint32_t destination, const Bytes& edited, const Context& context, std::string_view operation)
	{
		DirectoryGuard sourceGuard(Folder(activeId)), targetGuard(destination ? Folder(destination) : Folder(activeId)), backupGuard(backups);
		const auto body = Read(Body(activeId, id), s_bodyLimit), ext = Read(Ext(activeId, id), s_extLimit);
		Require(Version(Body(activeId, id), ext) == worlds.at(id).version, "stale-world");
		const auto bodyHash = Hash(body), extHash = Hash(ext);
		const auto bodyTime = fs::last_write_time(Body(activeId, id)), extTime = fs::last_write_time(Ext(activeId, id));
		if (destination)
		{
			Account(context, destination); Require(destination != activeId, "invalid-account"); CheckPath(Folder(destination));
			Require(!fs::exists(Body(destination, id)) && !fs::exists(Ext(destination, id)), "save-id-conflict");
		}
		Transaction transaction{std::string(operation), id, "", activeId, destination, bodyTime, extTime};
		const auto backup = Backup(transaction, body, ext, edited);
		const auto prefix = ".launcher-world-" + backup.filename().string();
		try
		{
			if (destination)
			{
				CheckPath(Folder(destination)); fs::create_directories(Folder(destination)); CheckPath(Folder(destination));
				DirectoryGuard destinationGuard(Folder(destination));
				const auto temporaryBody = Folder(destination) / (prefix + ".body.tmp"), temporaryExt = Folder(destination) / (prefix + ".ext.tmp");
				WriteNew(temporaryBody, body); WriteNew(temporaryExt, ext);
				Verify(temporaryBody, bodyHash, s_bodyLimit); Verify(temporaryExt, extHash, s_extLimit);
				Require(MoveFileExW(temporaryBody.c_str(), Body(destination, id).c_str(), MOVEFILE_WRITE_THROUGH), "write-failed");
				Require(MoveFileExW(temporaryExt.c_str(), Ext(destination, id).c_str(), MOVEFILE_WRITE_THROUGH), "write-failed");
				Verify(Body(destination, id), bodyHash, s_bodyLimit); Verify(Ext(destination, id), extHash, s_extLimit);
				WorldMetadata::Parse(Read(Ext(destination, id), s_extLimit));
				fs::last_write_time(Body(destination, id), bodyTime); fs::last_write_time(Ext(destination, id), extTime);
				WriteText(backup / "destination-verified", "verified");
				Verify(Body(activeId, id), bodyHash, s_bodyLimit); Verify(Ext(activeId, id), extHash, s_extLimit);
				RemoveKnown(Body(activeId, id), bodyHash, s_bodyLimit); RemoveKnown(Ext(activeId, id), extHash, s_extLimit);
				Verify(Body(destination, id), bodyHash, s_bodyLimit); Verify(Ext(destination, id), extHash, s_extLimit);
			}
			else
			{
				const auto temporary = Folder(activeId) / (prefix + ".ext.tmp");
				WriteNew(temporary, edited); Verify(temporary, Hash(edited), s_extLimit); WorldMetadata::Parse(Read(temporary, s_extLimit));
				Verify(Body(activeId, id), bodyHash, s_bodyLimit); Verify(Ext(activeId, id), extHash, s_extLimit);
				Require(ReplaceFileW(Ext(activeId, id).c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr), "write-failed");
				Verify(Ext(activeId, id), Hash(edited), s_extLimit); Verify(Body(activeId, id), bodyHash, s_bodyLimit);
			}
			WriteText(backup / "completed", "committed"); ClearMarker(); ++revision; notice = "saved";
		}
		catch (...)
		{
			// 既知のハッシュだけを復旧する。照合不能なら marker を残し追加操作を禁止する。
			try { Recover(context); notice = "rolled-back"; } catch (...) { notice = "recovery-required"; }
			throw std::runtime_error(notice);
		}
	}
	void RollBack(const Context& context)
	{
		try { Recover(context); notice = "rolled-back"; } catch (...) { notice = "recovery-required"; }
		throw std::runtime_error(notice);
	}
	void Delete(const std::string& id, const Context& context)
	{
		DirectoryGuard guard(Folder(activeId)); const auto body = Read(Body(activeId, id), s_bodyLimit), ext = Read(Ext(activeId, id), s_extLimit);
		Require(Version(Body(activeId, id), ext) == worlds.at(id).version, "stale-world");
		Transaction transaction{"delete", id, "", activeId, 0, fs::last_write_time(Body(activeId, id)), fs::last_write_time(Ext(activeId, id))};
		const auto backup = Backup(transaction, body, ext);
		try
		{
			CreateTrash(transaction, backup, body, ext);
			Verify(Body(activeId, id), Hash(body), s_bodyLimit); Verify(Ext(activeId, id), Hash(ext), s_extLimit);
			RemoveKnown(Body(activeId, id), Hash(body), s_bodyLimit); RemoveKnown(Ext(activeId, id), Hash(ext), s_extLimit);
			const auto entry = LoadTrash(transaction.trash); Require(entry.bodyHash == Hash(body) && entry.extHash == Hash(ext), "verification-failed");
			Receipt(trashRoot / transaction.trash / "deleted", transaction.trash); Receipt(backup / "completed", "committed"); ClearMarker(); ++revision; notice = "deleted";
		}
		catch (...) { RollBack(context); }
		ScanTrash();
	}
	void RefreshImportToken()
	{
		importToken = importSource && importSource->Preview().ready ? NewGuid() : "";
	}
	void Import(const Context& context)
	{
		Require(importSource != nullptr, "stale-import-preview");
		const auto id = importSource->Preview().id; const auto data = importSource->ReadVerified(); Account(context, activeId); CheckPath(Folder(activeId));
		Require(!fs::exists(Body(activeId, id)) && !fs::exists(Ext(activeId, id)), "save-id-conflict");
		Transaction transaction{"import", id, "", 0, activeId, data.bodyTime, data.extTime}; const auto backup = Backup(transaction, data.body, data.ext);
		try { CopyToAccount(transaction, backup, data.body, data.ext); Receipt(backup / "completed", "committed"); ClearMarker(); ++revision; notice = "imported"; }
		catch (...) { RollBack(context); }
		importSource.reset(); importToken.clear();
	}
	void Restore(const std::string& key, const std::string& version, const Context& context)
	{
		const auto entry = LoadTrash(key); Require(entry.version == version, "stale-trash");
		Require(entry.deleted && !entry.cancelled && entry.restored.empty(), "trash-unavailable"); Account(context, entry.account); CheckPath(Folder(entry.account));
		Require(!fs::exists(Body(entry.account, entry.id)) && !fs::exists(Ext(entry.account, entry.id)), "save-id-conflict");
		Transaction transaction{"restore", entry.id, key, 0, entry.account, fs::file_time_type(fs::file_time_type::duration(entry.bodyTime)), fs::file_time_type(fs::file_time_type::duration(entry.extTime))};
		const auto backup = Backup(transaction, entry.body, entry.ext);
		try
		{
			CopyToAccount(transaction, backup, entry.body, entry.ext); Receipt(trashRoot / key / "restored", backup.filename().string());
			Receipt(backup / "completed", "committed"); ClearMarker(); ++revision; notice = "restored";
		}
		catch (...) { RollBack(context); }
		ScanTrash();
	}
};

WorldService::WorldService(fs::path configRoot, fs::path saveRoot) : m_impl(std::make_unique<Impl>(std::move(configRoot), std::move(saveRoot))) {}
WorldService::~WorldService() = default;
bool WorldService::IsHealthy() const
{
	try { CheckPath(m_impl->marker); return m_impl->healthy && !fs::exists(m_impl->marker); }
	catch (...) { return false; }
}

std::string WorldService::Command(std::string_view payload, const Context& context)
{
	auto& state = *m_impl;
	try
	{
		DirectoryGuard configGuard(state.config), saveGuard(state.save);
		state.healthy = IsHealthy(); state.Scan(context);
		const auto json = Json(payload); Require(json.HasMember("op"), "invalid-request"); const auto op = String(json["op"]);
		if (op == "list" || op == "trash-list") { Require(json.MemberCount() == 1, "invalid-request"); if (op == "trash-list") state.ScanTrash(); if (state.healthy) state.notice = "ready"; }
		else if (op == "recover")
		{
			state.Validate(json, context, true); Require(json.MemberCount() == 3, "invalid-request"); state.Recover(context);
			state.ScanTrash();
		}
		else if (op == "prepare-purge" || op == "purge" || op == "clear-purge")
		{
			state.Validate(json, context, true); Require(state.healthy, "recovery-required");
			if (op == "prepare-purge") state.PreparePurge(json);
			else if (op == "clear-purge") { Require(json.MemberCount() == 3, "invalid-request"); state.purgePreview.reset(); state.purgeIssues.clear(); state.notice = "cancelled"; }
			else
			{
				Require(json.MemberCount() == 4 && json.HasMember("token") && state.purgePreview && String(json["token"]) == state.purgePreview->token, "stale-purge-preview");
				const auto plan = *state.purgePreview; state.ExecutePurge(plan, true);
			}
		}
		else if (op == "begin-import" || op == "name-import" || op == "clear-import-metadata")
		{
			state.Validate(json, context, true); Require(state.healthy, "recovery-required");
			if (op == "begin-import")
			{
				Require(json.MemberCount() == 3, "invalid-request"); state.importSource = std::make_unique<WorldImport>([](const fs::path& path, size_t limit) { DirectoryGuard guard(path.parent_path()); return Read(path, limit); });
			}
			else
			{
				Require(state.importSource != nullptr, "stale-import-preview");
				if (op == "name-import") { Require(json.MemberCount() == 4 && json.HasMember("name"), "invalid-request"); state.importToken.clear(); state.importSource->SetName(String(json["name"])); }
				else { Require(json.MemberCount() == 3, "invalid-request"); state.importToken.clear(); state.importSource->ClearMetadata(); }
			}
			state.RefreshImportToken(); state.notice = state.importSource->Preview().ready ? "import-preview-ready" : state.importSource->Preview().problem;
		}
		else if (op == "restore" || op == "import" || op == "clear-import")
		{
			state.Validate(json, context, true); Require(state.healthy, "recovery-required");
			if (op == "restore")
			{
				Require(json.MemberCount() == 5 && json.HasMember("trash") && json.HasMember("version"), "invalid-request"); state.Restore(String(json["trash"]), String(json["version"]), context);
			}
			else if (op == "import")
			{
				Require(json.HasMember("token") && json.HasMember("mode") && state.importSource && !state.importToken.empty() && String(json["token"]) == state.importToken, "stale-import-preview");
				const auto& candidate = state.importSource->Preview(); Require(candidate.ready && String(json["mode"]) == (candidate.generated ? "generated" : "metadata"), "invalid-import-mode");
				Require(json.MemberCount() == (candidate.generated ? 6 : 5) && (!candidate.generated || (json.HasMember("name") && String(json["name"]) == candidate.name)), "stale-import-preview");
				state.Import(context);
			}
			else { Require(json.MemberCount() == 3, "invalid-request"); state.importSource.reset(); state.importToken.clear(); state.notice = "cancelled"; }
		}
		else
		{
			const auto id = state.Validate(json, context);
			const auto ext = Read(state.Ext(context.activeId, id), s_extLimit);
			if (op == "rename")
			{
				Require(json.MemberCount() == 6 && json.HasMember("name"), "invalid-request");
				const auto edited = WorldMetadata::Rename(ext, String(json["name"]));
				if (edited == ext) state.notice = "unchanged";
				else state.Commit(id, 0, edited, context, "rename");
			}
			else if (op == "icon")
			{
				Require(json.MemberCount() == 6 && json.HasMember("token") && !state.preview.empty() && String(json["token"]) == state.previewToken &&
					id == state.previewWorld && state.worlds.at(id).version == state.previewVersion, "stale-preview");
				state.Commit(id, 0, state.preview, context, "icon");
			}
			else if (op == "move")
			{
				Require(json.MemberCount() == 6 && json.HasMember("destination") && json["destination"].IsUint(), "invalid-request");
				const auto destination = json["destination"].GetUint(); state.Account(context, destination);
				Require(destination != context.activeId, "invalid-account");
				state.Commit(id, destination, {}, context, "move");
			}
			else if (op == "delete") { Require(json.MemberCount() == 5, "invalid-request"); state.Delete(id, context); }
			else if (op == "clear-preview") { Require(json.MemberCount() == 5, "invalid-request"); state.notice = "cancelled"; }
			else Require(false, "invalid-request");
			state.preview.clear(); state.previewToken.clear();
		}
		state.Scan(context);
	}
	catch (const std::exception& error)
	{
		const std::set<std::string> safe{"invalid-request", "invalid-name", "unsupported-metadata", "not-ready", "recovery-required", "stale-request", "invalid-world", "stale-world", "stale-preview", "invalid-account", "save-id-conflict", "unsafe-path", "rolled-back", "read-failed", "write-failed", "invalid-file", "world-limit", "verification-failed", "insufficient-space", "invalid-trash", "stale-trash", "trash-unavailable", "trash-count-limit", "stale-import-preview", "stale-import-source", "invalid-body", "unsupported-body-format", "body-size-limit", "invalid-import-mode", "invalid-import-preview", "world-file-required", "import-icon-required", "metadata-id-mismatch", "trash-empty", "stale-purge-preview", "purge-blocked", "purge-backup-limit", "purge-changed", "purge-file-busy", "purge-write-failed", "invalid-purge", "unsafe-purge-file", "purge-unknown-file", "purge-plan-limit"};
		state.notice = !state.healthy ? "recovery-required" : safe.contains(error.what()) ? error.what() : "operation-failed";
	}
	return state.Snapshot(context);
}

std::string WorldService::PreviewImage(std::string_view payload, const fs::path& selectedFile, const Context& context)
{
	auto& state = *m_impl;
	try
	{
		DirectoryGuard configGuard(state.config), saveGuard(state.save);
		state.healthy = IsHealthy(); state.Scan(context); const auto json = Json(payload); const auto id = state.Validate(json, context);
		Require(json.MemberCount() == 5 && json.HasMember("op") && String(json["op"]) == "preview", "invalid-request");
		state.preview.clear(); state.previewToken.clear();
		if (selectedFile.empty()) state.notice = "cancelled";
		else
		{
			const auto ext = Read(state.Ext(context.activeId, id), s_extLimit);
			state.preview = WorldMetadata::ReplaceImage(ext, DecodeImage(selectedFile));
			state.previewToken = Hash(state.preview); state.previewWorld = id; state.previewVersion = state.worlds.at(id).version; state.notice = "preview-ready";
		}
	}
	catch (...) { state.preview.clear(); state.previewToken.clear(); state.notice = "invalid-image"; }
	return state.Snapshot(context);
}

std::string WorldService::PreviewImport(std::string_view payload, const fs::path& selected, std::string_view part, const Context& context)
{
	auto& state = *m_impl;
	try
	{
		DirectoryGuard configGuard(state.config), saveGuard(state.save); state.healthy = IsHealthy(); state.Scan(context);
		const auto json = Json(payload); state.Validate(json, context, true); Require(state.healthy, "recovery-required");
		Require(json.MemberCount() == 3 && json.HasMember("op") && String(json["op"]) == "select-import" && (part == "body" || part == "metadata" || part == "icon"), "invalid-request");
		Require(state.importSource != nullptr, "stale-import-preview");
		if (selected.empty()) state.notice = "cancelled";
		else
		{
			state.importToken.clear();
			if (part == "body") state.importSource->SelectBody(selected);
			else if (part == "metadata") state.importSource->SelectMetadata(selected);
			else { state.importSource->ClearIcon(); const auto display = selected.filename().u8string(); state.importSource->SetIcon(DecodeImage(selected), std::string(display.begin(), display.end())); }
			state.RefreshImportToken(); state.notice = state.importSource->Preview().ready ? "import-preview-ready" : state.importSource->Preview().problem;
		}
	}
	catch (const std::exception& error)
	{
		const std::set<std::string> safe{"not-ready","recovery-required","stale-request","invalid-request","stale-import-preview","unsafe-source-path","unsafe-path","invalid-save-id","unsupported-import-format","unsupported-metadata-file","metadata-id-mismatch","unsupported-title","read-failed","invalid-file","invalid-body","unsupported-body-format","body-size-limit","unsupported-metadata","invalid-image","invalid-import-mode"};
		state.notice = safe.contains(error.what()) ? error.what() : "invalid-import-source";
	}
	return state.Snapshot(context);
}
