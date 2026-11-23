#include "MiiInitializationStorage.h"
#include "MiiDatabase.h"
#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <span>
#include <stdexcept>
#include <map>
#include <mutex>

namespace fs = std::filesystem;
namespace
{
	constexpr uint64_t s_miiTitle = 0x000500101004a000ULL;
	constexpr std::array<const wchar_t*, 3> s_names{L"FFL_HDB.dat", L"FFL_ODB.dat", L"FFL_ODB_OLD.dat"};
	using Digest = std::array<unsigned char, 32>;
	using Cancel = MiiInitializationStorage::Cancel;
	void Require(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
	void CheckCancel(const Cancel& cancel) { Require(!cancel || !cancel(), "Mii initialization cancelled"); }
	bool Missing(DWORD error) { return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND; }
	fs::path NativePath(fs::path path)
	{
		Require(path.is_absolute(), "Unsafe Mii storage path");
		auto name = path.lexically_normal().make_preferred().native();
		if (name.starts_with(L"\\\\?\\")) return name;
		return name.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + name.substr(2) : L"\\\\?\\" + name;
	}
	struct Handle
	{
		HANDLE value = INVALID_HANDLE_VALUE;
		Handle() = default;
		explicit Handle(HANDLE v) : value(v) {}
		Handle(Handle&& other) noexcept : value(other.value) { other.value = INVALID_HANDLE_VALUE; }
		Handle& operator=(Handle&& other) noexcept
		{
			if (this != &other) { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); value = other.value; other.value = INVALID_HANDLE_VALUE; }
			return *this;
		}
		Handle(const Handle&) = delete;
		~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
	};
	void Ordinary(fs::path path)
	{
		const auto name = path.native();
		if (name.starts_with(L"\\\\?\\UNC\\")) path = L"\\\\" + name.substr(8);
		else if (name.starts_with(L"\\\\?\\")) path = name.substr(4);
		Require(path.is_absolute(), "Unsafe Mii storage path");
		for (; !path.empty(); path = path.parent_path())
		{
			const auto attributes = GetFileAttributesW(NativePath(path).c_str());
			if (attributes == INVALID_FILE_ATTRIBUTES)
			{
				const auto error = GetLastError();
				if (!Missing(error)) throw std::runtime_error(std::format("Cannot inspect Mii storage path (Windows error {})", error));
			}
			else Require(!(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "Linked Mii storage path rejected");
			if (path == path.root_path()) break;
		}
	}
	Handle Read(const fs::path& path, DWORD extraAccess = 0)
	{
		Ordinary(path);
		Handle file(CreateFileW(NativePath(path).c_str(), GENERIC_READ | extraAccess, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
		if (file.value == INVALID_HANDLE_VALUE) throw std::runtime_error(std::format("Cannot read Mii initialization data (Windows error {})", GetLastError()));
		BY_HANDLE_FILE_INFORMATION info{};
		Require(GetFileInformationByHandle(file.value, &info) && info.nNumberOfLinks == 1 && !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)), "Non-regular Mii initialization file");
		return file;
	}
	std::vector<unsigned char> Bytes(HANDLE file)
	{
		LARGE_INTEGER size{}, zero{};
		Require(GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart <= 1024 * 1024, "Invalid Mii initialization file length");
		Require(SetFilePointerEx(file, zero, nullptr, FILE_BEGIN), "Cannot seek Mii initialization data");
		std::vector<unsigned char> bytes(static_cast<size_t>(size.QuadPart));
		DWORD read = 0;
		Require(ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size(), "Cannot read complete Mii initialization data");
		return bytes;
	}
	Digest Hash(std::span<const unsigned char> bytes)
	{
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "Cannot initialize Mii data digest");
		Digest digest{};
		const auto status = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size()));
		BCryptCloseAlgorithmProvider(algorithm, 0);
		Require(status >= 0, "Cannot verify Mii data digest");
		return digest;
	}
	Digest FileHash(const fs::path& path) { auto file = Read(path); return Hash(Bytes(file.value)); }
	fs::path Database(const fs::path& mlc) { return mlc / L"usr/save/00050010/1004a000/user/common/db"; }
	std::string NewId()
	{
		std::array<unsigned char, 16> bytes{};
		Require(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0, "Cannot allocate Mii initialization identity");
		std::string result;
		for (auto b : bytes) result += std::format("{:02x}", b);
		return result;
	}
	void WriteAll(HANDLE file, std::span<const unsigned char> bytes)
	{
		DWORD written = 0;
		Require(WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size(), "Cannot write complete Mii initialization data");
		Require(FlushFileBuffers(file), "Cannot flush Mii initialization data");
	}
	Digest StreamHash(HANDLE file)
	{
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		BCRYPT_HASH_HANDLE hash = nullptr;
		Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "Cannot initialize ownership digest");
		const auto status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
		if (status < 0) { BCryptCloseAlgorithmProvider(algorithm, 0); throw std::runtime_error("Cannot initialize ownership digest"); }
		Digest result{};
		try
		{
			LARGE_INTEGER zero{};
			Require(SetFilePointerEx(file, zero, nullptr, FILE_BEGIN), "Cannot seek ownership input");
			std::array<unsigned char, 128 * 1024> buffer{};
			DWORD count = 0;
			do
			{
				Require(ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr), "Cannot read ownership input");
				if (count) Require(BCryptHashData(hash, buffer.data(), count, 0) >= 0, "Cannot digest ownership input");
			} while (count);
			Require(BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0) >= 0, "Cannot finish ownership digest");
		}
		catch (...) { BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); throw; }
		BCryptDestroyHash(hash);
		BCryptCloseAlgorithmProvider(algorithm, 0);
		return result;
	}
	void CopyNew(const fs::path& source, const fs::path& destination, const Cancel& cancel, const std::function<void(HANDLE, const Digest&)>& created = {})
	{
		CheckCancel(cancel);
		auto input = Read(source);
		Ordinary(destination);
		fs::create_directories(NativePath(destination.parent_path()));
		Ordinary(destination);
		Handle output(CreateFileW(NativePath(destination).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
		Require(output.value != INVALID_HANDLE_VALUE, "Cannot create Mii initialization copy");
		std::array<unsigned char, 128 * 1024> buffer{};
		while (true)
		{
			CheckCancel(cancel);
			DWORD read = 0, written = 0;
			Require(ReadFile(input.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr), "Cannot copy Mii initialization input");
			if (!read) break;
			Require(WriteFile(output.value, buffer.data(), read, &written, nullptr) && written == read, "Cannot copy complete Mii initialization input");
		}
		Require(FlushFileBuffers(output.value), "Cannot flush Mii initialization copy");
		if (created) created(output.value, StreamHash(input.value));
	}

	enum class Phase : uint32_t { Preparing = 1, Ready, Complete, Abandoned };
#pragma pack(push, 1)
	struct Journal
	{
		std::array<char, 8> magic{'M','C','W','M','I','I','0','1'};
		uint32_t version = 1;
		Phase phase = Phase::Preparing;
		std::array<char, 32> id{};
		std::array<uint8_t, 3> present{};
		std::array<Digest, 3> original{}, generated{};
		Digest checksum{};
	};
#pragma pack(pop)
	Digest JournalHash(const Journal& value) { return Hash({reinterpret_cast<const unsigned char*>(&value), offsetof(Journal, checksum)}); }
	bool ValidId(std::string_view id)
	{
		return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
	}
	std::string Key(const fs::path& path)
	{
		const auto name = path.generic_u8string();
		std::string key(reinterpret_cast<const char*>(name.data()), name.size());
		for (auto& c : key) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
		return key;
	}
	fs::path Utf8Path(std::string_view path)
	{
		return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(path.data()), path.size()));
	}
#pragma pack(push, 1)
	struct ObjectId
	{
		uint64_t volume = 0;
		std::array<unsigned char, 16> file{};
		bool operator==(const ObjectId&) const = default;
	};
	struct OwnerHeader
	{
		std::array<char, 8> magic{'M','C','W','O','W','N','0','1'};
		uint32_t version = 1, sequence = 0, state = 0, records = 0, metadata = 0;
		ObjectId directory, snapshot;
		Journal journal;
	};
	struct OwnerRecord
	{
		ObjectId object;
		Digest digest{};
		uint64_t size = 0;
		uint32_t flags = 0, pathBytes = 0;
	};
#pragma pack(pop)
	struct OwnedObject
	{
		ObjectId object;
		Digest digest{};
		uint64_t size = 0;
		bool directory = false, mutableFile = false;
	};
	ObjectId Identity(HANDLE file, bool directory)
	{
		FILE_ID_INFO id{};
		BY_HANDLE_FILE_INFORMATION info{};
		Require(GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) && GetFileInformationByHandle(file, &info), "Cannot identify owned Mii object");
		Require(!(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory && info.nNumberOfLinks == 1, "Linked or non-regular Mii cleanup object");
		ObjectId result;
		result.volume = id.VolumeSerialNumber;
		std::memcpy(result.file.data(), id.FileId.Identifier, result.file.size());
		return result;
	}
	uint64_t Length(HANDLE file)
	{
		LARGE_INTEGER size{};
		Require(GetFileSizeEx(file, &size) && size.QuadPart >= 0, "Cannot inspect owned Mii file size");
		return static_cast<uint64_t>(size.QuadPart);
	}
	Handle Directory(const fs::path& path, bool exclusive = false, bool deleting = false)
	{
		Ordinary(path);
		Handle file(CreateFileW(NativePath(path).c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | (deleting ? DELETE : 0), exclusive ? 0 : FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
		Require(file.value != INVALID_HANDLE_VALUE, "Cannot protect Mii cleanup directory");
		Identity(file.value, true);
		return file;
	}
	template<typename T> void Append(std::vector<unsigned char>& bytes, const T& value)
	{
		const auto* begin = reinterpret_cast<const unsigned char*>(&value);
		bytes.insert(bytes.end(), begin, begin + sizeof(value));
	}
	template<typename T> T Take(std::span<const unsigned char>& bytes)
	{
		Require(bytes.size() >= sizeof(T), "Truncated Mii ownership metadata");
		T value;
		std::memcpy(&value, bytes.data(), sizeof(value));
		bytes = bytes.subspan(sizeof(value));
		return value;
	}
	bool SnapshotName(std::string_view name)
	{
		return name.size() == 18 && name.starts_with("owner-") && name.ends_with(".dat") &&
			std::all_of(name.begin() + 6, name.begin() + 14, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
	}
}

struct MiiInitializationStorage::Impl
{
	fs::path data, mlc, root, working;
	Journal journal;
	Handle service;
	std::vector<Handle> originals;
	std::mutex ownershipMutex;
	std::map<std::string, OwnedObject> owned, metadata;
	ObjectId sessionDirectory;
	Digest ownerJournalChecksum{};
	uint32_t sequence = 0;
	bool ownershipActive = false, ownershipHealthy = true, sealed = false, cleaned = false;

	fs::path Session() const { return working.parent_path(); }
	std::string Relative(const fs::path& path) const
	{
		const auto relative = path.lexically_normal().lexically_relative(Session().lexically_normal());
		Require(!relative.empty() && relative != L"." && !relative.is_absolute(), "Unsafe Mii ownership path");
		for (const auto& part : relative) Require(part != L".." && part != L"." && part.native().find(L':') == std::wstring::npos, "Unsafe Mii ownership component");
		return Key(relative);
	}
	void CreateOwnedDirectories(const fs::path& path)
	{
		if (path == Session()) return;
		const auto key = Relative(path);
		if (const auto found = owned.find(key); found != owned.end())
		{
			auto directory = Directory(path);
			Require(found->second.directory && Identity(directory.value, true) == found->second.object, "Mii working directory was replaced");
			return;
		}
		CreateOwnedDirectories(path.parent_path());
		Require(CreateDirectoryW(NativePath(path).c_str(), nullptr), "Cannot exclusively create Mii working directory");
		auto directory = Directory(path);
		owned.emplace(key, OwnedObject{Identity(directory.value, true), {}, 0, true});
	}
	void CopyOwned(const fs::path& source, const fs::path& destination, const Cancel& cancel, bool mutableFile = false)
	{
		CreateOwnedDirectories(destination.parent_path());
		CopyNew(source, destination, cancel, [&](HANDLE file, const Digest& digest) {
			Require(owned.emplace(Relative(destination), OwnedObject{Identity(file, false), digest, Length(file), false, mutableFile}).second, "Duplicate Mii working file");
		});
	}
	void CopyOwnedTree(const fs::path& source, const fs::path& destination, const Cancel& cancel)
	{
		Ordinary(source);
		Require(fs::is_directory(source), "Required Mii initialization input directory is missing");
		CreateOwnedDirectories(destination);
		for (const auto& entry : fs::recursive_directory_iterator(source))
		{
			CheckCancel(cancel);
			Ordinary(entry.path());
			const auto target = destination / entry.path().lexically_relative(source);
			if (entry.is_directory()) CreateOwnedDirectories(target);
			else if (entry.is_regular_file()) CopyOwned(entry.path(), target, cancel);
			else throw std::runtime_error("Unsupported Mii working input");
		}
	}
	void SaveOwnership()
	{
		if (!ownershipActive || !ownershipHealthy) return;
		journal.checksum = JournalHash(journal);
		Require(sequence < 0xfffffff0 && owned.size() < 50000 && metadata.size() < 10000, "Mii ownership metadata limit reached");
		const auto name = std::format("owner-{:08x}.dat", ++sequence);
		Handle file(CreateFileW(NativePath(Session() / name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
		Require(file.value != INVALID_HANDLE_VALUE, "Cannot create immutable Mii ownership metadata");
		OwnerHeader header;
		header.sequence = sequence;
		header.state = cleaned ? 2 : sealed ? 1 : 0;
		header.records = static_cast<uint32_t>(owned.size());
		header.metadata = static_cast<uint32_t>(metadata.size());
		header.directory = sessionDirectory;
		header.snapshot = Identity(file.value, false);
		header.journal = journal;
		std::vector<unsigned char> bytes;
		Append(bytes, header);
		const auto append = [&](const auto& records) {
			for (const auto& [path, object] : records)
			{
				OwnerRecord record{object.object, object.digest, object.size, (object.directory ? 1u : 0u) | (object.mutableFile ? 2u : 0u), static_cast<uint32_t>(path.size())};
				Append(bytes, record);
				bytes.insert(bytes.end(), path.begin(), path.end());
			}
		};
		append(owned);
		append(metadata);
		Append(bytes, Hash(bytes));
		WriteAll(file.value, bytes);
		metadata.emplace(name, OwnedObject{header.snapshot, Hash(bytes), bytes.size()});
		ownerJournalChecksum = journal.checksum;
	}
	bool LoadOwnership(bool restoreJournal)
	{
		owned.clear(); metadata.clear(); sequence = 0;
		ownershipActive = false; ownershipHealthy = true; sealed = cleaned = false;
		Ordinary(Session());
		if (!fs::is_directory(Session())) return false;
		auto directory = Directory(Session());
		sessionDirectory = Identity(directory.value, true);
		std::string latest;
		for (const auto& entry : fs::directory_iterator(Session()))
		{
			const auto name = Key(entry.path().filename());
			if (SnapshotName(name) && name > latest) latest = name;
		}
		if (latest.empty()) return false;
		auto file = Read(Session() / latest);
		const auto bytes = Bytes(file.value);
		Require(bytes.size() >= sizeof(OwnerHeader) + sizeof(Digest), "Invalid Mii ownership metadata");
		std::span<const unsigned char> body(bytes.data(), bytes.size() - sizeof(Digest));
		Digest checksum{};
		std::memcpy(checksum.data(), bytes.data() + body.size(), checksum.size());
		Require(checksum == Hash(body), "Mii ownership checksum mismatch");
		const auto header = Take<OwnerHeader>(body);
		const std::string id(header.journal.id.begin(), header.journal.id.end());
		Require(header.magic == OwnerHeader{}.magic && header.version == 1 && header.state <= 2 && header.records < 50000 && header.metadata < 10000 &&
			ValidId(id) && Session().filename() == fs::path(id) && latest == std::format("owner-{:08x}.dat", header.sequence) &&
			header.directory == sessionDirectory && header.snapshot == Identity(file.value, false) &&
			header.journal.magic == Journal{}.magic && header.journal.version == 1 && header.journal.checksum == JournalHash(header.journal) &&
			header.journal.phase >= Phase::Preparing && header.journal.phase <= Phase::Abandoned &&
			std::all_of(header.journal.present.begin(), header.journal.present.end(), [](auto value) { return value <= 1; }), "Mii ownership identity mismatch");
		if (restoreJournal) journal = header.journal;
		else Require(journal.id == header.journal.id && journal.present == header.journal.present && journal.original == header.journal.original, "Mii ownership transaction mismatch");
		const auto take = [&](auto& records, uint32_t count, bool meta) {
			for (uint32_t i = 0; i < count; ++i)
			{
				const auto record = Take<OwnerRecord>(body);
				Require(record.flags <= 3 && record.pathBytes && record.pathBytes <= 4096 && body.size() >= record.pathBytes, "Invalid Mii ownership record");
				std::string path(reinterpret_cast<const char*>(body.data()), record.pathBytes);
				body = body.subspan(record.pathBytes);
				const auto physical = Session() / Utf8Path(path);
				Require(path.find('\0') == std::string::npos && Relative(physical) == path && (!meta || (SnapshotName(path) && !record.flags)) &&
					(meta || path == "mlc01" || path.starts_with("mlc01/") || (path.starts_with("addition-") && path.ends_with(".tmp") && path.find('/') == std::string::npos)), "Unsafe Mii cleanup record");
				Require(records.emplace(path, OwnedObject{record.object, record.digest, record.size, bool(record.flags & 1), bool(record.flags & 2)}).second, "Duplicate Mii cleanup record");
			}
		};
		take(owned, header.records, false);
		take(metadata, header.metadata, true);
		Require(body.empty(), "Unexpected Mii ownership data");
		metadata.emplace(latest, OwnedObject{header.snapshot, Hash(bytes), bytes.size()});
		sequence = header.sequence;
		ownerJournalChecksum = header.journal.checksum;
		sealed = header.state >= 1;
		cleaned = header.state == 2;
		ownershipActive = true;
		return true;
	}
	bool SealOwnership()
	{
		if (!ownershipActive || !ownershipHealthy) return false;
		try
		{
			for (auto& [path, object] : owned)
			{
				const auto physical = Session() / Utf8Path(path);
				if (object.directory)
				{
					auto directory = Directory(physical);
					Require(Identity(directory.value, true) == object.object, "Mii working directory changed before cleanup");
				}
				else
				{
					if (!fs::exists(physical)) continue;
					auto file = Read(physical);
					Require(Identity(file.value, false) == object.object, "Mii working file was replaced");
					const auto digest = StreamHash(file.value);
					if (!object.mutableFile) Require(object.digest == digest && object.size == Length(file.value), "Mii working input was modified");
					object.digest = digest;
					object.size = Length(file.value);
					object.mutableFile = false;
				}
			}
			for (const auto& entry : fs::recursive_directory_iterator(Session()))
			{
				Ordinary(entry.path());
				const auto key = Relative(entry.path());
				Require(owned.contains(key) || metadata.contains(key), "Unknown file in Mii working session; cleanup deferred");
			}
			sealed = true;
			SaveOwnership();
			return true;
		}
		catch (...) { ownershipHealthy = false; return false; }
	}
	MiiInitializationStorage::CleanupResult CleanupOwned(const Cancel& cancel)
	{
		MiiInitializationStorage::CleanupResult result;
		if (cleaned) return result;
		result.pending = 1;
		if (!ownershipActive || !ownershipHealthy || !sealed || (journal.phase != Phase::Complete && journal.phase != Phase::Abandoned))
		{ result.reason = "Mii cleanup ownership is not finalized"; return result; }
		try
		{
			const auto databases = MiiDatabase::Inspect(mlc, s_miiTitle);
			Require(!databases.HasBlockingError(), "Formal Mii data is invalid; cleanup deferred");
			if (journal.phase == Phase::Complete)
			{
				for (size_t i = 0; i < s_names.size(); ++i)
					Require(databases.files[i].state == MiiDatabase::State::Valid && FileHash(databases.files[i].path) == (journal.present[i] ? journal.original[i] : journal.generated[i]), "Formal Mii data changed; cleanup deferred");
				PreserveOriginals(true);
			}
			else PreserveOriginals(false);
			// 全オブジェクトを排他的に開いて検査してから削除する。パスによる再帰削除はしない。
			struct Locked
			{
				std::string path;
				bool directory, metadata;
				Handle file;
			};
			std::vector<Handle> ancestors;
			for (auto path = Session().parent_path(); !path.empty(); path = path.parent_path())
			{
				ancestors.push_back(Directory(path));
				if (path == data) break;
			}
			std::vector<Locked> locked;
			auto session = Directory(Session(), true);
			Require(Identity(session.value, true) == sessionDirectory, "Mii cleanup session was replaced");
			locked.push_back({{}, true, false, std::move(session)});
			const auto protect = [&](const auto& records, bool meta) {
				for (const auto& [path, object] : records)
				{
					const auto physical = Session() / Utf8Path(path);
					const auto attributes = GetFileAttributesW(NativePath(physical).c_str());
					if (attributes == INVALID_FILE_ATTRIBUTES)
					{
						Require(!meta && Missing(GetLastError()), "Cannot inspect Mii cleanup object");
						continue;
					}
					const auto access = object.directory ? FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | DELETE : GENERIC_READ | (meta ? 0 : DELETE);
					Handle file(CreateFileW(NativePath(physical).c_str(), access, 0, nullptr, OPEN_EXISTING,
						FILE_FLAG_OPEN_REPARSE_POINT | (object.directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_FLAG_SEQUENTIAL_SCAN), nullptr));
					Require(file.value != INVALID_HANDLE_VALUE, "Mii cleanup object is busy or inaccessible");
					Require(Identity(file.value, object.directory) == object.object && !object.mutableFile, "Mii cleanup object was replaced");
					if (!object.directory) Require(Length(file.value) == object.size && StreamHash(file.value) == object.digest, "Mii cleanup file changed");
					locked.push_back({path, object.directory, meta, std::move(file)});
				}
			};
			protect(owned, false);
			protect(metadata, true);
			for (auto& object : locked)
			{
				if (!object.directory) continue;
				std::array<unsigned char, 65536> buffer{};
				auto query = FileIdBothDirectoryRestartInfo;
				while (true)
				{
					if (!GetFileInformationByHandleEx(object.file.value, query, buffer.data(), static_cast<DWORD>(buffer.size())))
					{
						Require(GetLastError() == ERROR_NO_MORE_FILES, "Cannot enumerate protected Mii directory");
						break;
					}
					query = FileIdBothDirectoryInfo;
					const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data());
					while (true)
					{
						const fs::path name(std::wstring(entry->FileName, entry->FileNameLength / sizeof(wchar_t)));
						if (name != L"." && name != L"..")
						{
							const auto key = Key(Utf8Path(object.path) / name);
							Require(owned.contains(key) || metadata.contains(key), "Unknown Mii session object; cleanup deferred");
						}
						if (!entry->NextEntryOffset) break;
						entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(reinterpret_cast<const unsigned char*>(entry) + entry->NextEntryOffset);
					}
				}
			}
			std::sort(locked.begin(), locked.end(), [](const auto& a, const auto& b) {
				if (a.directory != b.directory) return !a.directory;
				return a.path.size() > b.path.size();
			});
			for (auto& object : locked)
			{
				if (object.metadata || object.path.empty()) continue;
				CheckCancel(cancel);
				FILE_STANDARD_INFO info{};
				Require(GetFileInformationByHandleEx(object.file.value, FileStandardInfo, &info, sizeof(info)), "Cannot measure Mii cleanup file");
				FILE_DISPOSITION_INFO disposition{TRUE};
				Require(SetFileInformationByHandle(object.file.value, FileDispositionInfo, &disposition, sizeof(disposition)), "Mii cleanup deletion deferred");
				CloseHandle(object.file.value); object.file.value = INVALID_HANDLE_VALUE;
				if (!object.directory) result.bytes += static_cast<uint64_t>(info.AllocationSize.QuadPart);
			}
			// 所有情報は小さいtombstoneとして残す。途中停止時は旧snapshotから再開できる。
			owned.clear();
			cleaned = true;
			SaveOwnership();
			result.pending = 0;
		}
		catch (const std::exception& error) { result.reason = error.what(); }
		return result;
	}
	MiiInitializationStorage::CleanupResult Sweep()
	{
		MiiInitializationStorage::CleanupResult result;
		for (const auto& entry : fs::directory_iterator(root))
		{
			const auto id = Key(entry.path().filename());
			if (!ValidId(id)) continue;
			try
			{
				working = root / id / L"mlc01";
				if (!LoadOwnership(true)) continue;
				if (journal.phase != Phase::Complete && journal.phase != Phase::Abandoned) continue;
				const auto cleanup = CleanupOwned({});
				result.bytes += cleanup.bytes;
				result.pending += cleanup.pending;
				if (!cleanup.reason.empty()) result.reason = cleanup.reason;
			}
			catch (const std::exception& error) { ++result.pending; result.reason = error.what(); }
		}
		return result;
	}
	explicit Impl(fs::path directory) : data(std::move(directory)), mlc(data / L"mlc01"), root(data / L".mii-initialization")
	{
		Ordinary(root);
		fs::create_directories(root);
		Ordinary(root);
		service.value = CreateFileW(NativePath(root / L"service.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		Require(service.value != INVALID_HANDLE_VALUE, "Mii initialization is busy");
	}
	void Save()
	{
		journal.checksum = JournalHash(journal);
		const auto temporary = root / (L"journal-" + fs::path(NewId()).native() + L".tmp");
		Handle file(CreateFileW(NativePath(temporary).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_FLAG_WRITE_THROUGH, nullptr));
		Require(file.value != INVALID_HANDLE_VALUE, "Cannot create Mii initialization journal");
		WriteAll(file.value, {reinterpret_cast<const unsigned char*>(&journal), sizeof(journal)});
		CloseHandle(file.value); file.value = INVALID_HANDLE_VALUE;
		Ordinary(root / L"journal.dat");
		Require(MoveFileExW(NativePath(temporary).c_str(), NativePath(root / L"journal.dat").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "Cannot commit Mii initialization journal");
		if (ownershipActive && ownershipHealthy)
		{
			try { SaveOwnership(); }
			catch (...) { ownershipHealthy = false; }
		}
	}
	bool Load()
	{
		const auto path = root / L"journal.dat";
		if (!fs::exists(path)) return false;
		auto file = Read(path);
		const auto bytes = Bytes(file.value);
		Require(bytes.size() == sizeof(journal), "Mii initialization recovery requires attention");
		std::memcpy(&journal, bytes.data(), sizeof(journal));
		Require(journal.magic == Journal{}.magic && journal.version == 1 && journal.checksum == JournalHash(journal) &&
			journal.phase >= Phase::Preparing && journal.phase <= Phase::Abandoned &&
			std::all_of(journal.present.begin(), journal.present.end(), [](auto value) { return value <= 1; }) &&
			std::all_of(journal.id.begin(), journal.id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }), "Invalid Mii initialization journal; existing data preserved");
		working = root / std::string(journal.id.begin(), journal.id.end()) / L"mlc01";
		Ordinary(working);
		try { LoadOwnership(false); }
		catch (...) { ownershipActive = false; ownershipHealthy = false; }
		return true;
	}
	void PreserveOriginals(bool allowNew)
	{
		const auto state = MiiDatabase::Inspect(mlc, s_miiTitle);
		Require(!state.HasBlockingError(), "Mii data is invalid or unreadable; existing files preserved");
		for (size_t i = 0; i < s_names.size(); ++i)
		{
			if (journal.present[i])
			{
				Require(state.files[i].state == MiiDatabase::State::Valid, "Existing Mii data changed during initialization");
				auto file = Read(state.files[i].path);
				Require(Hash(Bytes(file.value)) == journal.original[i], "Existing Mii data changed during initialization");
				originals.push_back(std::move(file));
			}
			else if (state.files[i].state != MiiDatabase::State::Missing)
				Require(allowNew && FileHash(state.files[i].path) == journal.generated[i], "Mii destination changed; existing files preserved");
		}
	}
	void Finish(const Cancel& cancel)
	{
		CheckCancel(cancel);
		const auto generated = MiiDatabase::Inspect(working, s_miiTitle);
		Require(!generated.HasBlockingError() && std::all_of(generated.files.begin(), generated.files.end(), [](const auto& file) { return file.state == MiiDatabase::State::Valid; }), "Generated Mii data failed validation");
		for (size_t i = 0; i < s_names.size(); ++i)
			Require(FileHash(generated.files[i].path) == journal.generated[i] && (!journal.present[i] || journal.generated[i] == journal.original[i]), "Mii initialization data changed");
		PreserveOriginals(true);
		const auto target = Database(mlc);
		Ordinary(target);
		fs::create_directories(target);
		// 親ディレクトリの置換を反映中だけ禁止する。
		std::vector<Handle> directories;
		for (auto path = target; path != data && !path.empty(); path = path.parent_path())
		{
			Ordinary(path);
			Handle directory(CreateFileW(NativePath(path).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
			Require(directory.value != INVALID_HANDLE_VALUE, "Cannot protect Mii destination directory");
			directories.push_back(std::move(directory));
		}
		for (size_t i = 0; i < s_names.size(); ++i)
		{
			CheckCancel(cancel);
			if (journal.present[i]) continue;
			const auto destination = target / s_names[i];
			if (fs::exists(destination))
				Require(FileHash(destination) == journal.generated[i], "Mii destination changed; recovery required");
			else
			{
				// 完全なコピーを同一volumeでflushし、保持したhandleで上書きなしにrenameする。
				// 中断された作業ファイルは正式DBへ移さず、そのまま保護する。
				const auto staged = working.parent_path() / (L"addition-" + fs::path(NewId()).native() + L".tmp");
				if (ownershipActive && ownershipHealthy) CopyOwned(generated.files[i].path, staged, {});
				else CopyNew(generated.files[i].path, staged, {});
				if (ownershipActive && ownershipHealthy) SaveOwnership();
				auto candidate = Read(staged, DELETE);
				Require(Hash(Bytes(candidate.value)) == journal.generated[i], "Staged Mii data changed");
				const auto name = NativePath(destination).native();
				std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t));
				auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
				rename->ReplaceIfExists = FALSE;
				rename->RootDirectory = nullptr;
				rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
				std::memcpy(rename->FileName, name.data(), rename->FileNameLength);
				Require(SetFileInformationByHandle(candidate.value, FileRenameInfo, rename, static_cast<DWORD>(buffer.size())), "Mii destination changed or cannot be added; recovery required");
			}
			Require(FileHash(destination) == journal.generated[i], "Added Mii data failed verification");
		}
		Require(MiiDatabase::Inspect(mlc, s_miiTitle).HasRequiredDatabases(), "Mii initialization recovery requires attention");
		journal.phase = Phase::Complete;
		Save();
	}
};

MiiInitializationStorage::CleanupResult MiiInitializationStorage::Recover(const fs::path& data)
{
	if (!fs::exists(data / L".mii-initialization")) return {};
	Impl state(data);
	if (state.Load())
	{
		if (state.journal.phase == Phase::Ready) state.Finish({});
		else if (state.journal.phase == Phase::Preparing)
		{
			// Preparingでは正式DBを作らない。未確定の所有情報は削除許可にしない。
			state.PreserveOriginals(false);
			state.journal.phase = Phase::Abandoned;
			state.Save();
		}
		else if (state.ownershipActive && state.ownershipHealthy && state.ownerJournalChecksum != state.journal.checksum)
			state.SaveOwnership();
	}
	return state.Sweep();
}

MiiInitializationStorage::MiiInitializationStorage(const fs::path& data, const std::vector<fs::path>& titles, Cancel cancel)
	: m_impl(std::make_unique<Impl>(data))
{
	auto& state = *m_impl;
	if (state.Load()) Require(state.journal.phase == Phase::Complete || state.journal.phase == Phase::Abandoned, "Pending Mii initialization recovery");
	state.journal = {};
	state.owned.clear(); state.metadata.clear(); state.sequence = 0;
	state.sealed = state.cleaned = false;
	state.ownershipHealthy = true;
	const auto id = NewId();
	std::copy(id.begin(), id.end(), state.journal.id.begin());
	state.working = state.root / id / L"mlc01";
	const auto inspection = MiiDatabase::Inspect(state.mlc, s_miiTitle);
	Require(inspection.NeedsInitialization(), "Mii initialization prerequisites are not available");
	for (size_t i = 0; i < s_names.size(); ++i)
		if (inspection.files[i].state == MiiDatabase::State::Valid)
		{
			state.journal.present[i] = 1;
			state.journal.original[i] = FileHash(inspection.files[i].path);
		}
	state.PreserveOriginals(false);
	Require(CreateDirectoryW(NativePath(state.Session()).c_str(), nullptr), "Cannot exclusively create Mii session");
	{ auto directory = Directory(state.Session()); state.sessionDirectory = Identity(directory.value, true); }
	state.ownershipActive = true;
	state.Save();
	CheckCancel(cancel);
	uintmax_t required = 64 * 1024 * 1024;
	for (const auto& title : titles)
		for (const auto& entry : fs::recursive_directory_iterator(title))
		{
			CheckCancel(cancel);
			Ordinary(entry.path());
			if (entry.is_regular_file()) required += entry.file_size();
		}
	Require(fs::space(state.root).available > required, "Not enough space to initialize Mii data safely");
	for (const auto& title : titles)
	{
		const auto relative = title.lexically_relative(state.mlc);
		Require(!relative.empty() && *relative.begin() != L".." && relative.generic_string().starts_with("usr/title/"), "Unsafe Mii initialization title path");
		state.CopyOwnedTree(title, state.working / relative, cancel);
	}
	state.CopyOwnedTree(state.mlc / L"sys/title/0005001b/10056000", state.working / L"sys/title/0005001b/10056000", cancel);
	state.CopyOwnedTree(state.mlc / L"usr/save/system/act", state.working / L"usr/save/system/act", cancel);
	state.CreateOwnedDirectories(Database(state.working));
	for (size_t i = 0; i < s_names.size(); ++i)
		if (state.journal.present[i]) state.CopyOwned(inspection.files[i].path, Database(state.working) / s_names[i], cancel);
	state.SaveOwnership();
}

MiiInitializationStorage::~MiiInitializationStorage() = default;
const fs::path& MiiInitializationStorage::WorkingMlc() const { return m_impl->working; }

void MiiInitializationStorage::Commit(Cancel cancel)
{
	auto& state = *m_impl;
	CheckCancel(cancel);
	const auto inspection = MiiDatabase::Inspect(state.working, s_miiTitle);
	Require(!inspection.HasBlockingError() && std::all_of(inspection.files.begin(), inspection.files.end(), [](const auto& file) { return file.state == MiiDatabase::State::Valid; }), "Generated Mii data failed validation");
	for (size_t i = 0; i < s_names.size(); ++i) state.journal.generated[i] = FileHash(inspection.files[i].path);
	state.SealOwnership();
	state.journal.phase = Phase::Ready;
	state.Save();
	state.Finish(cancel);
}

void MiiInitializationStorage::PrepareGuestDirectories(uint32_t persistentId)
{
	auto& state = *m_impl;
	const auto id = std::format("{:08x}", persistentId);
	for (const auto& path : {"usr/boss/00050000/101dbe00/user/" + id, std::string("usr/boss/00050000/101dbe00/user/common"),
		"usr/save/00050000/101dbe00/user/" + id, std::string("usr/save/00050000/101dbe00/user/common"),
		std::string("usr/save/00050000/101dbe00/meta"), std::string("usr/save/system/pdm/80000001")})
		state.CreateOwnedDirectories(state.working / path);
	for (const auto name : {"meta.xml", "iconTex.tga"})
	{
		auto source = state.working / L"usr/title/0005000e/101dbe00/meta" / name;
		if (!fs::exists(source)) source = state.working / L"usr/title/00050000/101dbe00/meta" / name;
		if (fs::exists(source)) state.CopyOwned(source, state.working / L"usr/save/00050000/101dbe00/meta" / name, {}, true);
	}
	state.SaveOwnership();
}

void MiiInitializationStorage::ObserveCreated(const fs::path& path, void* file) noexcept
{
	auto& state = *m_impl;
	std::lock_guard lock(state.ownershipMutex);
	try
	{
		// FileStreamの作成handleがrenameを拒否している間にIDを取得する。
		const auto relative = path.lexically_normal().lexically_relative(state.Session().lexically_normal());
		if (relative.empty() || *relative.begin() == L".." || relative.is_absolute()) return;
		const auto key = state.Relative(path);
		Require(state.owned.contains(Key(relative.parent_path())) && state.owned.at(Key(relative.parent_path())).directory, "Unowned directory created during Mii initialization");
		Require(state.owned.emplace(key, OwnedObject{Identity(file, false), {}, 0, false, true}).second, "Mii initialization file identity changed");
		state.SaveOwnership();
	}
	catch (...) { state.ownershipHealthy = false; }
}

MiiInitializationStorage::CleanupResult MiiInitializationStorage::Cleanup(Cancel cancel)
{
	return m_impl->CleanupOwned(cancel);
}

MiiInitializationStorage::CleanupResult MiiInitializationStorage::Abandon()
{
	auto& state = *m_impl;
	if (state.journal.phase == Phase::Complete) return state.CleanupOwned({});
	if (state.journal.phase != Phase::Preparing) return {0, 1};
	try
	{
		state.PreserveOriginals(false);
		state.SealOwnership();
		state.journal.phase = Phase::Abandoned;
		state.Save();
		return state.CleanupOwned({});
	}
	catch (...) { return {0, 1}; }
}
