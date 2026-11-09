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
	rapidjson::Document Json(std::string_view payload)
	{
		Require(!payload.empty() && payload.size() <= 8192 && payload.find('\0') == std::string_view::npos, "invalid-request");
		rapidjson::Document json; json.Parse<rapidjson::kParseValidateEncodingFlag>(payload.data(), payload.size());
		Require(!json.HasParseError() && json.IsObject(), "invalid-request");
		std::set<std::string> names;
		for (auto it = json.MemberBegin(); it != json.MemberEnd(); ++it) Require(names.insert(String(it->name)).second, "invalid-request");
		return json;
	}
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
	fs::path config, save, marker, backups;
	uint64_t revision = 1, accountRevision = 0;
	uint32_t activeId = 0;
	std::string notice = "ready", previewToken, previewWorld, previewVersion;
	Bytes preview;
	bool healthy = true;
	struct World
	{
		std::string id, name, icon, version, problem;
		uint64_t size = 0;
	};
	std::map<std::string, World> worlds;
	Impl(fs::path configRoot, fs::path saveRoot) : config(std::move(configRoot)), save(std::move(saveRoot)), marker(config / ".launcher-world-transaction"), backups(config / ".launcher-world-backups")
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
			activeId = context.activeId; accountRevision = context.accountRevision; ++revision; preview.clear(); previewToken.clear();
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
	fs::path Backup(const std::string& id, uint32_t destination, std::span<const uint8_t> body, std::span<const uint8_t> ext, std::span<const uint8_t> edited)
	{
		CheckPath(backups); fs::create_directories(backups); CheckPath(backups);
		GUID guid{}; Require(SUCCEEDED(CoCreateGuid(&guid)), "write-failed");
		wchar_t identifier[40]{}; StringFromGUID2(guid, identifier, 40);
		const fs::path folder = backups / identifier; Require(fs::create_directory(folder), "write-failed");
		WriteNew(folder / "body", body); WriteNew(folder / "metadata", ext);
		if (!edited.empty()) WriteNew(folder / "edited", edited);
		rapidjson::StringBuffer buffer; Writer writer(buffer); writer.StartObject();
		Text(writer, "world", id); writer.Key("source"); writer.Uint(activeId); writer.Key("destination"); writer.Uint(destination);
		Text(writer, "bodyHash", Hash(body)); Text(writer, "extHash", Hash(ext)); Text(writer, "editedHash", edited.empty() ? "" : Hash(edited));
		writer.Key("bodySize"); writer.Uint64(body.size()); writer.Key("extSize"); writer.Uint64(ext.size());
		writer.Key("bodyTime"); writer.Int64(fs::last_write_time(Body(activeId, id)).time_since_epoch().count());
		writer.Key("extTime"); writer.Int64(fs::last_write_time(Ext(activeId, id)).time_since_epoch().count());
		writer.EndObject(); WriteText(folder / "manifest.json", buffer.GetString());
		Verify(folder / "body", Hash(body), s_bodyLimit); Verify(folder / "metadata", Hash(ext), s_extLimit);
		healthy = false; WriteText(marker, folder.filename().string());
		return folder;
	}
	void ClearMarker()
	{
		CheckPath(marker); Require(DeleteFileW(marker.c_str()), "write-failed"); healthy = true;
	}
	void Recover(const Context& context)
	{
		Require(!healthy && fs::exists(marker), "recovery-required");
		const auto markerBytes = Read(marker, 128);
		const std::string id(markerBytes.begin(), markerBytes.end());
		Require(id.size() == 38 && id.front() == '{' && id.back() == '}' && std::all_of(id.begin() + 1, id.end() - 1, [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') || c == '-'; }), "recovery-required");
		const auto folder = backups / id; const auto manifestBytes = Read(folder / "manifest.json", 8192);
		const auto json = Json({reinterpret_cast<const char*>(manifestBytes.data()), manifestBytes.size()});
		Require(json.MemberCount() == 10 && json.HasMember("world") && json.HasMember("source") && json["source"].IsUint() && json.HasMember("destination") && json["destination"].IsUint() && json.HasMember("bodyHash") && json.HasMember("extHash") && json.HasMember("editedHash") &&
			json.HasMember("bodySize") && json["bodySize"].IsUint64() && json.HasMember("extSize") && json["extSize"].IsUint64() &&
			json.HasMember("bodyTime") && json["bodyTime"].IsInt64() && json.HasMember("extTime") && json["extTime"].IsInt64(), "recovery-required");
		const auto world = String(json["world"]); Require(SaveId(world), "recovery-required");
		const auto source = json["source"].GetUint(), destination = json["destination"].GetUint();
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
	void Commit(const std::string& id, uint32_t destination, const Bytes& edited, const Context& context)
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
		const auto backup = Backup(id, destination, body, ext, edited);
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
		if (op == "list") { Require(json.MemberCount() == 1, "invalid-request"); if (state.healthy) state.notice = "ready"; }
		else if (op == "recover")
		{
			state.Validate(json, context, true); Require(json.MemberCount() == 3, "invalid-request"); state.Recover(context);
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
				else state.Commit(id, 0, edited, context);
			}
			else if (op == "icon")
			{
				Require(json.MemberCount() == 6 && json.HasMember("token") && !state.preview.empty() && String(json["token"]) == state.previewToken &&
					id == state.previewWorld && state.worlds.at(id).version == state.previewVersion, "stale-preview");
				state.Commit(id, 0, state.preview, context);
			}
			else if (op == "move")
			{
				Require(json.MemberCount() == 6 && json.HasMember("destination") && json["destination"].IsUint(), "invalid-request");
				const auto destination = json["destination"].GetUint(); state.Account(context, destination);
				Require(destination != context.activeId, "invalid-account");
				state.Commit(id, destination, {}, context);
			}
			else if (op == "clear-preview") { Require(json.MemberCount() == 5, "invalid-request"); state.notice = "cancelled"; }
			else Require(false, "invalid-request");
			state.preview.clear(); state.previewToken.clear();
		}
		state.Scan(context);
	}
	catch (const std::exception& error)
	{
		const std::set<std::string> safe{"invalid-request", "invalid-name", "unsupported-metadata", "not-ready", "recovery-required", "stale-request", "invalid-world", "stale-world", "stale-preview", "invalid-account", "save-id-conflict", "unsafe-path", "rolled-back", "read-failed", "write-failed", "invalid-file", "world-limit", "verification-failed"};
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
