#include "GameInstallationService.h"
#include "LauncherPaths.h"

#include <Windows.h>
#include <bcrypt.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace fs = std::filesystem;
using Service = GameInstallationService;
using Error = Service::Error;
using Path = Service::Path;

namespace
{
	constexpr std::array<uint32_t, 3> s_titleLow{0x101dbe00, 0x101d9d00, 0x101d7500};
	constexpr std::array<uint32_t, 3> s_titleHigh{0x00050000, 0x0005000e, 0x0005000c};
	constexpr wchar_t s_owner[] = L".mcwiiu-launcher-title";
	struct Failure : std::runtime_error
	{
		Error code;
		Failure(Error error, std::string message) : std::runtime_error(std::move(message)), code(error) {}
	};
	void Require(bool condition, Error code, const char* message)
	{
		if (!condition) throw Failure(code, message);
	}
	void WinCheck(bool success, const char* message)
	{
		if (success) return;
		const auto code = GetLastError();
		throw Failure(code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL ? Error::NoSpace :
			code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION ? Error::AccessDenied : Error::IO, std::string(message) + " (Win32 " + std::to_string(code) + ")");
	}
	struct Handle
	{
		HANDLE value = INVALID_HANDLE_VALUE;
		explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
		~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
		Handle(const Handle&) = delete;
		Handle& operator=(const Handle&) = delete;
		Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
		Handle& operator=(Handle&& other) noexcept
		{
			if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
			value = std::exchange(other.value, INVALID_HANDLE_VALUE);
			return *this;
		}
	};
#pragma pack(push, 1)
	struct Identity
	{
		uint32_t volume = 0;
		uint64_t index = 0;
		bool operator==(const Identity&) const = default;
		explicit operator bool() const { return index != 0; }
	};
	struct Item
	{
		uint64_t titleId = 0;
		Identity oldDirectory, newDirectory;
	};
	struct Journal
	{
		char magic[8] = {'M', 'C', 'W', 'I', 'N', 'S', 'T', '1'};
		uint32_t version = 1, operation = 0, count = 0;
		Identity dataDirectory, transactionDirectory;
		Item items[9]{};
		std::array<unsigned char, 32> digest{};
	};
#pragma pack(pop)

#ifdef GAME_INSTALLATION_TESTING
	std::function<void(std::string_view, uint64_t)> s_checkpoint;
	void Checkpoint(std::string_view name, uint64_t value = 0) { if (s_checkpoint) s_checkpoint(name, value); }
#else
	void Checkpoint(std::string_view, uint64_t = 0) {}
#endif

	bool Equal(const Path& left, const Path& right) { return _wcsicmp(left.c_str(), right.c_str()) == 0; }
	bool Within(const Path& child, const Path& parent)
	{
		auto first = child.begin(), second = parent.begin();
		for (; second != parent.end(); ++first, ++second)
			if (first == child.end() || !Equal(*first, *second)) return false;
		return true;
	}
	Path LongPath(Path path) { return Path(L"\\\\?\\" + path.make_preferred().native()); }
	void ValidatePath(const Path& path)
	{
		Require(path.is_absolute() && path.root_name().native().size() == 2 && path.native().size() < 32000,
			Error::InvalidPath, "Select an absolute local Windows path");
		for (const auto& part : path.relative_path())
		{
			const auto name = part.native();
			Require(!name.empty() && name != L"." && name != L".." && name.size() <= 255 && name.back() != L'.' && name.back() != L' ' &&
				name.find_first_of(L"<>:\"|?*") == std::wstring::npos, Error::InvalidPath, "Unsafe Windows path component");
		}
	}
	DWORD Attributes(const Path& path)
	{
		const auto attributes = GetFileAttributesW(LongPath(path).c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES)
		{
			const auto error = GetLastError();
			if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return attributes;
			WinCheck(false, "Cannot inspect title path");
		}
		Require(!(attributes & FILE_ATTRIBUTE_REPARSE_POINT), Error::UnsafePath, "Links and junctions are not allowed");
		return attributes;
	}
	bool Exists(const Path& path) { return Attributes(path) != INVALID_FILE_ATTRIBUTES; }
	Handle Open(const Path& path, DWORD access, DWORD disposition = OPEN_EXISTING, DWORD sharing = FILE_SHARE_READ)
	{
		Handle handle(CreateFileW(LongPath(path).c_str(), access, sharing, nullptr, disposition,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
		WinCheck(handle.value != INVALID_HANDLE_VALUE, "Cannot open installation file or directory");
		BY_HANDLE_FILE_INFORMATION info{};
		WinCheck(GetFileInformationByHandle(handle.value, &info), "Cannot inspect installation handle");
		Require(!(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
			((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || info.nNumberOfLinks == 1), Error::UnsafePath, "Linked installation files are not allowed");
		return handle;
	}
	Identity FileIdentity(HANDLE handle)
	{
		BY_HANDLE_FILE_INFORMATION info{};
		WinCheck(GetFileInformationByHandle(handle, &info), "Cannot read installation identity");
		return {info.dwVolumeSerialNumber, (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow};
	}
	Identity DirectoryIdentity(const Path& path)
	{
		const auto attributes = Attributes(path);
		if (attributes == INVALID_FILE_ATTRIBUTES) return {};
		Require(attributes & FILE_ATTRIBUTE_DIRECTORY, Error::UnsafePath, "Expected an ordinary title directory");
		auto handle = Open(path, FILE_READ_ATTRIBUTES);
		return FileIdentity(handle.value);
	}
	struct Locks
	{
		std::vector<Handle> handles;
		void Add(const Path& path)
		{
			Require(Attributes(path) != INVALID_FILE_ATTRIBUTES && (Attributes(path) & FILE_ATTRIBUTE_DIRECTORY), Error::InvalidPath, "Title directory is missing");
			handles.push_back(Open(path, FILE_READ_ATTRIBUTES));
		}
		void Ancestors(const Path& path)
		{
			ValidatePath(path);
			Path current = path.root_path();
			Add(current);
			for (const auto& part : path.relative_path()) { current /= part; Add(current); }
		}
	};
	void MakeDirectories(const Path& path)
	{
		ValidatePath(path);
		Path current = path.root_path();
		Locks locks;
		locks.Add(current);
		for (const auto& part : path.relative_path())
		{
			current /= part;
			if (!Exists(current)) WinCheck(CreateDirectoryW(LongPath(current).c_str(), nullptr), "Cannot create installation directory");
			locks.Add(current);
		}
	}
	template<typename Function> void Children(const Path& directory, Function&& function)
	{
		for (const auto& entry : fs::directory_iterator(LongPath(directory)))
		{
			const auto path = directory / entry.path().filename();
			ValidatePath(path);
			function(path);
		}
	}
	uint64_t Size(HANDLE handle)
	{
		LARGE_INTEGER size{};
		WinCheck(GetFileSizeEx(handle, &size), "Cannot read installation file size");
		return static_cast<uint64_t>(size.QuadPart);
	}
	std::vector<unsigned char> ReadSmall(const Path& path, size_t limit)
	{
		Locks locks; locks.Ancestors(path.parent_path());
		auto handle = Open(path, GENERIC_READ);
		const auto size = Size(handle.value);
		Require(size <= limit, Error::InvalidMetadata, "Installation metadata is too large");
		std::vector<unsigned char> bytes(static_cast<size_t>(size));
		DWORD read = 0;
		WinCheck(ReadFile(handle.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr), "Cannot read installation metadata");
		Require(read == bytes.size(), Error::IO, "Installation metadata changed while reading");
		return bytes;
	}
	void WriteDurable(const Path& path, const void* bytes, DWORD size)
	{
		auto handle = Open(path, GENERIC_WRITE, CREATE_NEW, 0);
		DWORD written = 0;
		WinCheck(WriteFile(handle.value, bytes, size, &written, nullptr) && written == size && FlushFileBuffers(handle.value), "Cannot persist installation transaction");
	}
	void Move(const Path& source, const Path& destination, Identity expected)
	{
		Require(DirectoryIdentity(source) == expected && expected && !Exists(destination), Error::RecoveryRequired, "Installation directories changed unexpectedly");
		// 同一 volume の rename のみ。置換、copy fallback、再起動時の遅延操作は使わない。
		WinCheck(MoveFileExW(LongPath(source).c_str(), LongPath(destination).c_str(), MOVEFILE_WRITE_THROUGH), "Cannot move installation directory");
		Require(DirectoryIdentity(destination) == expected, Error::RecoveryRequired, "Installation identity changed after rename");
	}
	void DeleteTree(const Path& path, Identity expected = {})
	{
		if (!Exists(path)) return;
		auto handle = Open(path, DELETE | FILE_READ_ATTRIBUTES);
		if (expected) Require(FileIdentity(handle.value) == expected, Error::RecoveryRequired, "Refusing to delete an unowned installation directory");
		if (Attributes(path) & FILE_ATTRIBUTE_DIRECTORY) Children(path, [&](const Path& child) { DeleteTree(child); });
		FILE_DISPOSITION_INFO disposition{TRUE};
		WinCheck(SetFileInformationByHandle(handle.value, FileDispositionInfo, &disposition, sizeof(disposition)), "Cannot remove owned installation data");
	}
	std::array<unsigned char, 32> Digest(const void* bytes, size_t size)
	{
		std::array<unsigned char, 32> result{};
		Require(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<void*>(bytes)),
			static_cast<ULONG>(size), result.data(), static_cast<ULONG>(result.size())) == 0, Error::IO, "Cannot checksum installation journal");
		return result;
	}
	std::string Hex(uint64_t number, size_t width)
	{
		char buffer[17]{};
		const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), number, 16);
		std::string result(buffer, converted.ptr);
		return std::string(width > result.size() ? width - result.size() : 0, '0') + result;
	}
	uint64_t Number(std::wstring_view text, int base, uint64_t maximum)
	{
		Require(!text.empty() && text.size() <= 16, Error::InvalidMetadata, "Invalid title number");
		uint64_t value = 0;
		for (auto character : text)
		{
			const int digit = character >= L'0' && character <= L'9' ? character - L'0' :
				character >= L'a' && character <= L'f' ? character - L'a' + 10 : character >= L'A' && character <= L'F' ? character - L'A' + 10 : -1;
			Require(digit >= 0 && digit < base && value <= (maximum - digit) / base, Error::InvalidMetadata, "Invalid title number");
			value = value * base + digit;
		}
		return value;
	}
	std::map<std::wstring, std::wstring> ReadXml(const Path& path, std::wstring_view root)
	{
		const auto bytes = ReadSmall(path, 1024 * 1024);
		using Microsoft::WRL::ComPtr;
		ComPtr<IStream> stream;
		stream.Attach(SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
		ComPtr<IXmlReader> reader;
		Require(stream && SUCCEEDED(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr)), Error::InvalidMetadata, "Cannot create title XML reader");
		Require(SUCCEEDED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) &&
			SUCCEEDED(reader->SetProperty(XmlReaderProperty_MaxElementDepth, 32)) && SUCCEEDED(reader->SetInput(stream.Get())), Error::InvalidMetadata, "Cannot initialize title XML reader");
		std::map<std::wstring, std::wstring> values;
		std::wstring field;
		bool foundRoot = false;
		XmlNodeType type;
		HRESULT status;
		while ((status = reader->Read(&type)) == S_OK)
		{
			UINT depth = 0; reader->GetDepth(&depth);
			const wchar_t* value = nullptr;
			UINT length = 0;
			if (type == XmlNodeType_Element)
			{
				reader->GetLocalName(&value, &length);
				if (!depth)
				{
					Require(!foundRoot && std::wstring_view(value, length) == root, Error::InvalidMetadata, "Invalid title XML root");
					foundRoot = true;
				}
				field.clear();
				if (depth == 1)
				{
					const std::wstring_view name(value, length);
					if (name == L"title_id" || name == L"title_version" || name == L"region" || name == L"argstr" || name == L"longname_ja" || name == L"longname_en")
					{
						field = name;
						Require(values.emplace(field, L"").second, Error::InvalidMetadata, "Duplicate title XML field");
					}
				}
				if (reader->IsEmptyElement()) field.clear();
			}
			else if ((type == XmlNodeType_Text || type == XmlNodeType_CDATA || type == XmlNodeType_Whitespace) && depth == 2 && !field.empty())
			{
				reader->GetValue(&value, &length);
				Require(values[field].size() + length <= 256, Error::InvalidMetadata, "Title XML field is too large");
				values[field].append(value, length);
			}
			else if (type == XmlNodeType_EndElement) field.clear();
		}
		Require(status == S_FALSE && foundRoot, Error::InvalidMetadata, "Invalid title XML or prohibited DTD");
		for (auto& [key, value] : values)
		{
			const auto start = value.find_first_not_of(L" \t\r\n"), end = value.find_last_not_of(L" \t\r\n");
			value = start == std::wstring::npos ? L"" : value.substr(start, end - start + 1);
		}
		return values;
	}
	Service::Title Describe(uint64_t id)
	{
		const auto low = std::find(s_titleLow.begin(), s_titleLow.end(), static_cast<uint32_t>(id));
		const auto high = std::find(s_titleHigh.begin(), s_titleHigh.end(), static_cast<uint32_t>(id >> 32));
		// TitleIdParser、GraphicPack2::IsMinecraftTitle と同じ allowlist。Core をリンクしない。
		Require(low != s_titleLow.end() && high != s_titleHigh.end(), Error::UnsupportedTitle, "Only Minecraft Wii U JPN, USA and EUR titles are supported");
		Service::Title title;
		title.id = id;
		title.region = static_cast<Service::Region>(1 + std::distance(s_titleLow.begin(), low));
		title.type = static_cast<Service::TitleType>(std::distance(s_titleHigh.begin(), high));
		return title;
	}
	Service::Title Metadata(const Path& source, const Path& data, bool requireV560 = true)
	{
		Locks locks; locks.Ancestors(source);
		for (const auto& part : {L"code", L"content", L"meta"}) locks.Add(source / part);
		auto app = ReadXml(source / L"code/app.xml", L"app");
		auto meta = ReadXml(source / L"meta/meta.xml", L"menu");
		auto title = Describe(Number(app[L"title_id"], 16, UINT64_MAX));
		Require(app[L"title_id"].size() == 16 && meta[L"title_id"].size() == 16, Error::InvalidMetadata, "Title IDs must have 16 hexadecimal digits");
		title.version = static_cast<uint16_t>(Number(app[L"title_version"], 16, UINT16_MAX));
		const auto metaId = Number(meta[L"title_id"], 16, UINT64_MAX);
		const auto baseId = (uint64_t(s_titleHigh[0]) << 32) | static_cast<uint32_t>(title.id);
		Require((metaId == title.id || (title.type == Service::TitleType::Update && metaId == baseId)) &&
			Number(meta[L"title_version"], 10, UINT16_MAX) == title.version, Error::InvalidMetadata, "app.xml and meta.xml disagree");
		ReadXml(source / L"code/cos.xml", L"app");
		if (title.type != Service::TitleType::DLC)
		{
			auto executable = Open(source / L"code/Minecraft.Client.rpx", GENERIC_READ);
			Require(Size(executable.value) != 0, Error::InvalidMetadata, "Minecraft executable is missing or empty");
		}
		if (requireV560 && title.type == Service::TitleType::Update) Require(title.version == 560, Error::UnsupportedVersion, "Only update v560 is supported");
		const auto& name = meta[L"longname_ja"].empty() ? meta[L"longname_en"] : meta[L"longname_ja"];
		if (!name.empty())
		{
			const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
			Require(count > 0, Error::InvalidMetadata, "Invalid title name encoding");
			title.name.resize(count);
			WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), title.name.data(), count, nullptr, nullptr);
		}
		title.source = source;
		title.destination = Service::TitlePath(data, title.id);
		return title;
	}
	std::string Owner(uint64_t id) { return "MCWiiU-Launcher title v1\n" + Hex(id, 16) + "\n"; }
	void CheckOwner(const Path& title, uint64_t id)
	{
		Require(Exists(title / s_owner), Error::UnmanagedTitle, "Existing title is not owned by this installer; it was preserved");
		const auto bytes = ReadSmall(title / s_owner, 128);
		const auto expected = Owner(id);
		Require(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()) == expected, Error::UnmanagedTitle, "Unrecognized title ownership marker; original preserved");
	}
	struct File { Path relative; uint64_t size = 0; bool directory = false; };
	void Inventory(const Path& source, const Path& relative, Locks& locks, std::vector<File>& files, uint64_t& bytes, const std::atomic_bool* cancelled)
	{
		if (cancelled) Require(!*cancelled, Error::Cancelled, "Installation cancelled");
		const auto path = source / relative;
		if (Attributes(path) & FILE_ATTRIBUTE_DIRECTORY)
		{
			locks.Add(path);
			files.push_back({relative, 0, true});
			Children(path, [&](const Path& child) { Inventory(source, relative / child.filename(), locks, files, bytes, cancelled); });
		}
		else
		{
			auto handle = Open(path, GENERIC_READ);
			const auto size = Size(handle.value);
			Require(bytes <= UINT64_MAX - size, Error::InvalidMetadata, "Installation size overflow");
			bytes += size;
			files.push_back({relative, size});
		}
	}
} // namespace

struct GameInstallationService::Impl
{
	const LauncherPaths& paths;
	Path transactions, preparing;
	Handle exclusive;
	Locks anchors;
	std::mutex lifecycle, storage;
	mutable std::mutex mutex;
	std::thread worker;
	std::condition_variable finished;
	std::atomic_bool cancelled{false};
	bool initialized = false, busy = false, gameActive = false;
	Snapshot snapshot;

	explicit Impl(const LauncherPaths& value) : paths(value), transactions(paths.data / L".launcher-install/transactions"), preparing(paths.data / L".launcher-install/preparing") {}
	static Result Exception()
	{
		try { throw; }
		catch (const Failure& failure) { return {failure.code, failure.what()}; }
		catch (const fs::filesystem_error& failure)
		{
			const auto error = failure.code().value();
			return {error == ERROR_DISK_FULL ? Error::NoSpace : error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION ? Error::AccessDenied : Error::IO, "Installation filesystem operation failed"};
		}
		catch (const std::exception&) { return {Error::IO, "Installation operation failed"}; }
	}
	void Publish(Phase phase, uint64_t titleId = 0)
	{
		std::lock_guard lock(mutex);
		snapshot.progress.phase = phase;
		snapshot.progress.titleId = titleId;
		snapshot.progress.cancellable = phase == Phase::Analyzing || phase == Phase::Planning || phase == Phase::Copying;
	}
	Snapshot ReadInstalled()
	{
		Snapshot result;
		result.legacyDataPresent = Exists(paths.data / L"games/base");
		const Title* base = nullptr;
		const Title* update = nullptr;
		result.titles.reserve(9);
		for (const auto high : s_titleHigh) for (const auto low : s_titleLow)
		{
			const uint64_t id = (uint64_t(high) << 32) | low;
			const auto destination = TitlePath(paths.data, id);
			if (!Exists(destination)) continue;
			CheckOwner(destination, id);
			auto title = Metadata(destination, paths.data, false);
			Require(title.id == id, Error::UnmanagedTitle, "Installed title does not match its MLC directory");
			title.relation = Relation::Installed;
			result.titles.push_back(title);
			if (title.type == TitleType::Base)
			{
				Require(!base, Error::RegionMismatch, "Multiple BASE regions are installed; all data was preserved");
				base = &result.titles.back();
			}
			if (title.type == TitleType::Update)
			{
				Require(!update, Error::RegionMismatch, "Multiple Update regions are installed; all data was preserved");
				update = &result.titles.back();
			}
		}
		Require(!update || (base && base->region == update->region), Error::RegionMismatch, "Installed BASE and Update regions disagree");
		if (base)
		{
			result.region = base->region;
			result.basePath = base->destination;
			result.ready = update && update->version == 560;
			// World/Texture の JPN 固定契約は Phase 4 で拡張する。
			result.launchSupported = result.ready && result.region == Region::JPN;
			result.state = result.ready ? State::Ready : update ? State::UpdateMissing : State::BaseOnly;
		}
		return result;
	}
	Analysis Analyze(const Path& selected, const std::atomic_bool* cancel) const
	{
		Analysis result;
		try
		{
			ValidatePath(selected);
			Path source = selected;
			if (!(Attributes(selected) & FILE_ATTRIBUTE_DIRECTORY))
			{
				Require(Equal(selected.filename(), L"meta.xml") && Equal(selected.parent_path().filename(), L"meta"), Error::InvalidPath, "Select a title folder or its meta/meta.xml");
				source = selected.parent_path().parent_path();
			}
			Require(Exists(source), Error::InvalidPath, "Installation source does not exist");
			const auto install = paths.data / L".launcher-install", mlc = paths.data / L"mlc01";
			Require(!Within(source, mlc) && !Within(mlc, source) && !Within(source, install) && !Within(install, source), Error::UnsafePath, "Installation source overlaps managed storage");
			result.title = Metadata(source, paths.data);
			Locks locks; locks.Ancestors(source);
			std::vector<File> files;
			for (const auto& part : {L"code", L"content", L"meta"}) Inventory(source, part, locks, files, result.title.bytes, cancel);
			result.title.files = std::count_if(files.begin(), files.end(), [](const File& file) { return !file.directory; });
			if (Exists(result.title.destination)) result.title.relation = Relation::Installed;
			else
			{
				for (const auto low : s_titleLow)
					if (low != static_cast<uint32_t>(result.title.id) && Exists(TitlePath(paths.data, (uint64_t(s_titleHigh[0]) << 32) | low))) result.title.relation = Relation::OtherRegion;
			}
		}
		catch (...) { result.result = Exception(); }
		return result;
	}
	void ValidateJournal(const Journal& journal, const Path& transaction)
	{
		const Journal empty;
		Require(std::memcmp(journal.magic, empty.magic, sizeof(journal.magic)) == 0 && journal.version == 1 && journal.count && journal.count <= 9 &&
			journal.operation >= static_cast<uint32_t>(Operation::Install) && journal.operation <= static_cast<uint32_t>(Operation::RemoveDLC) &&
			journal.digest == Digest(&journal, offsetof(Journal, digest)) && journal.dataDirectory == DirectoryIdentity(paths.data) &&
			journal.transactionDirectory == DirectoryIdentity(transaction), Error::InvalidJournal, "Invalid installation journal; transaction data was preserved");
		for (uint32_t index = 0; index < journal.count; ++index)
		{
			const auto& item = journal.items[index];
			Describe(item.titleId);
			Require((item.oldDirectory || item.newDirectory) && (!item.oldDirectory || item.oldDirectory.volume == journal.dataDirectory.volume) &&
				(!item.newDirectory || item.newDirectory.volume == journal.dataDirectory.volume) &&
				(!item.oldDirectory || item.oldDirectory != item.newDirectory), Error::InvalidJournal, "Invalid installation directory identities");
			for (uint32_t other = 0; other < index; ++other) Require(journal.items[other].titleId != item.titleId, Error::InvalidJournal, "Duplicate transaction title");
		}
		// ジャーナルに記載していない子は、取引が所有すると推測して削除しない。
		Children(transaction, [&](const Path& child) {
			const auto name = child.filename().native();
			Require(name == L"plan.bin" || name == L"commit" || name == L"commit.tmp" || name == L"rollback" || name == L"rollback.tmp" ||
				name == L"cleaned" || name == L"cleaned.tmp" || name == L"stage" || name == L"backup", Error::InvalidJournal, "Unexpected transaction file; preserved");
			Require(((Attributes(child) & FILE_ATTRIBUTE_DIRECTORY) != 0) == (name == L"stage" || name == L"backup"), Error::InvalidJournal, "Unexpected transaction entry type; preserved");
		});
		for (const auto& folder : {L"stage", L"backup"})
		{
			Require(static_cast<bool>(DirectoryIdentity(transaction / folder)), Error::InvalidJournal, "Missing transaction container");
			Children(transaction / folder, [&](const Path& child) {
				bool known = false;
				for (uint32_t index = 0; index < journal.count; ++index) if (child.filename() == std::to_wstring(index)) known = true;
				Require(known, Error::InvalidJournal, "Unexpected transaction payload; preserved");
			});
		}
	}
	bool Marker(const Path& transaction, const wchar_t* name, const Journal& journal)
	{
		const auto path = transaction / name;
		if (!Exists(path)) return false;
		const auto bytes = ReadSmall(path, journal.digest.size());
		Require(bytes.size() == journal.digest.size() && std::equal(bytes.begin(), bytes.end(), journal.digest.begin()), Error::InvalidJournal, "Invalid transaction decision marker; preserved");
		return true;
	}
	void Mark(const Path& transaction, const wchar_t* name, const Journal& journal)
	{
		if (Marker(transaction, name, journal)) return;
		const auto temporary = transaction / (std::wstring(name) + L".tmp");
		if (Exists(temporary)) DeleteTree(temporary);
		WriteDurable(temporary, journal.digest.data(), static_cast<DWORD>(journal.digest.size()));
		WinCheck(MoveFileExW(LongPath(temporary).c_str(), LongPath(transaction / name).c_str(), MOVEFILE_WRITE_THROUGH), "Cannot persist transaction decision");
	}
	Path Payload(const Path& transaction, const wchar_t* folder, uint32_t index) { return transaction / folder / std::to_wstring(index); }
	void Recover(const Path& transaction)
	{
		const auto bytes = ReadSmall(transaction / L"plan.bin", sizeof(Journal));
		Require(bytes.size() == sizeof(Journal), Error::InvalidJournal, "Incomplete installation journal; preserved");
		Journal journal;
		std::memcpy(&journal, bytes.data(), sizeof(journal));
		ValidateJournal(journal, transaction);
		const bool committed = Marker(transaction, L"commit", journal), rolledBack = Marker(transaction, L"rollback", journal);
		Require(!committed || !rolledBack, Error::InvalidJournal, "Conflicting installation decisions; preserved");
		if (Marker(transaction, L"cleaned", journal))
		{
			Require(committed || rolledBack, Error::InvalidJournal, "Missing transaction decision");
			Children(transaction / L"stage", [](const Path&) { throw Failure(Error::InvalidJournal, "Cleaned transaction still has data"); });
			Children(transaction / L"backup", [](const Path&) { throw Failure(Error::InvalidJournal, "Cleaned transaction still has data"); });
			return;
		}
		// 全 entry の組合せを検査してから復旧する。rename と記録の間の停止も ID で判別する。
		for (uint32_t index = 0; index < journal.count; ++index)
		{
			const auto& item = journal.items[index];
			const auto target = DirectoryIdentity(TitlePath(paths.data, item.titleId));
			const auto backup = DirectoryIdentity(Payload(transaction, L"backup", index));
			const auto stage = DirectoryIdentity(Payload(transaction, L"stage", index));
			Require((!backup || (item.oldDirectory && backup == item.oldDirectory)) && (!stage || (item.newDirectory && stage == item.newDirectory)), Error::RecoveryRequired, "Unknown transaction payload; preserved");
			if (committed) Require(target == item.newDirectory && !stage, Error::RecoveryRequired, "Committed installation changed unexpectedly; preserved");
			else if (rolledBack) Require(target == item.oldDirectory && !backup, Error::RecoveryRequired, "Rolled-back installation changed unexpectedly; preserved");
			else
			{
				Require(!target || target == item.oldDirectory || target == item.newDirectory, Error::RecoveryRequired, "Unknown installed directory; preserved");
				Require(!item.oldDirectory || ((target == item.oldDirectory) != (backup == item.oldDirectory)), Error::RecoveryRequired, "Previous installation could not be identified");
				Require(!item.newDirectory || ((target == item.newDirectory) != (stage == item.newDirectory)), Error::RecoveryRequired, "Staged installation could not be identified");
			}
		}
		if (!committed && !rolledBack)
		{
			Publish(Phase::RollingBack);
			for (uint32_t remaining = journal.count; remaining; --remaining)
			{
				const uint32_t index = remaining - 1;
				const auto& item = journal.items[index];
				const auto target = TitlePath(paths.data, item.titleId);
				if (item.newDirectory && DirectoryIdentity(target) == item.newDirectory)
				{
					Move(target, Payload(transaction, L"stage", index), item.newDirectory);
					Checkpoint("rollback-new", index);
				}
				const auto backup = Payload(transaction, L"backup", index);
				if (Exists(backup)) { Move(backup, target, item.oldDirectory); Checkpoint("rollback-old", index); }
			}
			Mark(transaction, L"rollback", journal);
		}
		Publish(Phase::Cleaning);
		for (uint32_t index = 0; index < journal.count; ++index)
		{
			const auto& item = journal.items[index];
			DeleteTree(Payload(transaction, L"stage", index), item.newDirectory);
			DeleteTree(Payload(transaction, L"backup", index), item.oldDirectory);
			Checkpoint("cleanup", index);
		}
		// 小さな完了記録を残す。journal 自体の削除中断による曖昧な復旧を避ける。
		Mark(transaction, L"cleaned", journal);
	}
	void RecoverAll()
	{
		Children(transactions, [&](const Path& transaction) {
			const auto name = transaction.filename().native();
			Require(name.size() == 36 && name.find_first_not_of(L"0123456789abcdef-") == std::wstring::npos, Error::InvalidJournal, "Unknown transaction directory; preserved");
			Recover(transaction);
		});
	}
	void CopyTitle(const Title& title, const Path& stage)
	{
		Publish(Phase::Copying, title.id);
		Locks locks; locks.Ancestors(title.source);
		std::vector<File> files;
		uint64_t total = 0;
		for (const auto& part : {L"code", L"content", L"meta"}) Inventory(title.source, part, locks, files, total, &cancelled);
		Require(total == title.bytes && static_cast<uint64_t>(std::count_if(files.begin(), files.end(), [](const File& file) { return !file.directory; })) == title.files,
			Error::CopyFailed, "Source changed after installation planning");
		std::vector<unsigned char> buffer(1024 * 1024);
		for (const auto& file : files)
		{
			Require(!cancelled, Error::Cancelled, "Installation cancelled");
			const auto destination = stage / file.relative;
			if (file.directory) { MakeDirectories(destination); continue; }
			auto input = Open(title.source / file.relative, GENERIC_READ);
			Require(Size(input.value) == file.size, Error::CopyFailed, "Source file size changed");
			auto output = Open(destination, GENERIC_READ | GENERIC_WRITE, CREATE_NEW, 0);
			uint64_t copied = 0;
			while (copied < file.size)
			{
				Require(!cancelled, Error::Cancelled, "Installation cancelled");
				DWORD read = 0, written = 0;
				const auto amount = static_cast<DWORD>(std::min<uint64_t>(buffer.size(), file.size - copied));
				WinCheck(ReadFile(input.value, buffer.data(), amount, &read, nullptr), "Cannot read installation source");
				Require(read == amount, Error::CopyFailed, "Short read while copying installation source");
				Checkpoint("copy-write", copied);
				WinCheck(WriteFile(output.value, buffer.data(), read, &written, nullptr), "Cannot copy installation file");
				Require(written == read, Error::CopyFailed, "Short write while copying installation source");
				copied += written;
				{
					std::lock_guard lock(mutex);
					auto& progress = snapshot.progress;
					progress.processedBytes += written;
					progress.percent = progress.totalBytes ? 100.0 * static_cast<double>(progress.processedBytes) / static_cast<double>(progress.totalBytes) : 0;
				}
				Checkpoint("copy-chunk", copied);
			}
			WinCheck(FlushFileBuffers(output.value), "Cannot flush copied installation file");
			Require(Size(output.value) == file.size, Error::CopyFailed, "Copied installation file size disagrees");
		}
		const auto owner = Owner(title.id);
		WriteDurable(stage / s_owner, owner.data(), static_cast<DWORD>(owner.size()));
		const auto copied = Metadata(stage, paths.data);
		Require(copied.id == title.id && copied.version == title.version, Error::CopyFailed, "Copied title metadata disagrees");
		Checkpoint("copy-complete", title.id);
	}
	void Execute(const Request& request)
	{
		RecoverAll();
		const auto installed = ReadInstalled();
		std::vector<Title> titles;
		Publish(Phase::Analyzing);
		Require(request.sources.size() <= 3, Error::InvalidMetadata, "Select at most three installation sources");
		Require(request.expectedSources.empty() || request.expectedSources.size() == request.sources.size(), Error::InvalidMetadata, "Invalid source identity count");
		for (size_t index = 0; index < request.sources.size(); ++index)
		{
			auto analysis = Analyze(request.sources[index], &cancelled);
			if (!analysis.result) throw Failure(analysis.result.error, analysis.result.message.c_str());
			if (!request.expectedSources.empty()) Require(analysis.title.id == request.expectedSources[index].id && analysis.title.version == request.expectedSources[index].version,
				Error::InvalidMetadata, "Selected title changed after review; select and review the source again");
			for (const auto& other : titles) Require(other.id != analysis.title.id, Error::InvalidMetadata, "Duplicate input title");
			titles.push_back(std::move(analysis.title));
		}
		Publish(Phase::Planning);
		const bool removing = request.operation == Operation::Uninstall || request.operation == Operation::RemoveDLC;
		if (removing) Require(titles.empty(), Error::InvalidMetadata, "Removal does not accept installation sources");
		else Require(!titles.empty() && titles.size() <= 3, Error::InvalidMetadata, "Select installation sources");
		Region region = Region::None;
		bool hasBase = false, hasUpdate = false;
		for (const auto& title : titles)
		{
			if (title.type == TitleType::DLC) continue;
			Require(region == Region::None || region == title.region, Error::RegionMismatch, "Selected BASE and Update regions disagree");
			region = title.region;
			if (title.type == TitleType::Base) { Require(!hasBase, Error::RegionMismatch, "Select one BASE region"); hasBase = true; }
			else { Require(!hasUpdate, Error::RegionMismatch, "Select one Update region"); hasUpdate = true; }
		}
		if (request.operation == Operation::ChangeRegion)
			Require(titles.size() == 2 && hasBase && hasUpdate && installed.region != Region::None && region != installed.region, Error::RegionMismatch, "Region change requires a new BASE and matching v560 Update");
		else if (!removing)
		{
			Require(region == Region::None || installed.region == Region::None || installed.region == region, Error::RegionMismatch, "Use a region-change transaction for another BASE region");
			Require(!hasUpdate || hasBase || installed.region == region, Error::RegionMismatch, "Update requires its matching BASE");
			if (request.operation == Operation::ReinstallBase || request.operation == Operation::ReinstallUpdate)
			{
				const auto expected = request.operation == Operation::ReinstallBase ? TitleType::Base : TitleType::Update;
				Require(titles.size() == 1 && titles.front().type == expected && Exists(titles.front().destination), Error::NotInstalled, "Reinstallation requires the installed title of the same type and region");
			}
			else
				for (const auto& title : titles) Require(!Exists(title.destination), Error::AlreadyInstalled, "Title already installed; use an explicit reinstallation operation");
		}
		Journal journal;
		journal.operation = static_cast<uint32_t>(request.operation);
		journal.dataDirectory = DirectoryIdentity(paths.data);
		for (const auto& title : installed.titles)
		{
			if (request.operation == Operation::Uninstall || (request.operation == Operation::RemoveDLC && title.type == TitleType::DLC && title.id == request.dlcTitleId) ||
				(request.operation == Operation::ChangeRegion && title.type != TitleType::DLC))
				journal.items[journal.count++] = {title.id, DirectoryIdentity(title.destination), {}};
		}
		if (request.operation == Operation::RemoveDLC)
			Require(Describe(request.dlcTitleId).type == TitleType::DLC && journal.count == 1, Error::NotInstalled, "Selected DLC is not installed");
		uint64_t required = 0;
		for (const auto& title : titles)
		{
			Require(required <= UINT64_MAX - title.bytes, Error::NoSpace, "Installation size overflow");
			required += title.bytes;
			journal.items[journal.count++] = {title.id, DirectoryIdentity(title.destination), {}};
		}
		if (!journal.count) return;
		Require(journal.count <= 9, Error::InvalidMetadata, "Too many installation titles");
		{
			std::lock_guard lock(mutex);
			snapshot.progress.totalBytes = required;
		}
		Checkpoint("space-check", required);
		const auto space = fs::space(LongPath(paths.data));
		Require(required <= UINT64_MAX - 16 * 1024 * 1024 && space.available >= required + 16 * 1024 * 1024, Error::NoSpace, "Insufficient space for staging and journal; current titles were preserved");
		Require(!cancelled, Error::Cancelled, "Installation cancelled");
		GUID guid{};
		Require(SUCCEEDED(CoCreateGuid(&guid)), Error::IO, "Cannot create installation transaction ID");
		wchar_t formatted[40]{};
		StringFromGUID2(guid, formatted, 40);
		std::wstring name(formatted + 1, 36);
		std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
		const auto draft = preparing / name, transaction = transactions / name;
		Require(!Exists(draft) && !Exists(transaction), Error::IO, "Installation transaction already exists");
		MakeDirectories(draft / L"stage"); MakeDirectories(draft / L"backup");
		journal.transactionDirectory = DirectoryIdentity(draft);
		for (uint32_t index = 0; index < journal.count; ++index)
		{
			for (const auto& title : titles) if (journal.items[index].titleId == title.id)
			{
				const auto stage = Payload(draft, L"stage", index);
				MakeDirectories(stage);
				journal.items[index].newDirectory = DirectoryIdentity(stage);
			}
		}
		journal.digest = Digest(&journal, offsetof(Journal, digest));
		WriteDurable(draft / L"plan.bin", &journal, sizeof(journal));
		Move(draft, transaction, journal.transactionDirectory);
		Checkpoint("journal-published");
		try
		{
			for (uint32_t index = 0; index < journal.count; ++index)
				for (const auto& title : titles) if (journal.items[index].titleId == title.id) CopyTitle(title, Payload(transaction, L"stage", index));
			Require(!cancelled, Error::Cancelled, "Installation cancelled");
			Publish(Phase::Placing);
			for (uint32_t index = 0; index < journal.count; ++index)
			{
				const auto& item = journal.items[index];
				const auto target = TitlePath(paths.data, item.titleId);
				Require(DirectoryIdentity(target) == item.oldDirectory, Error::RecoveryRequired, "Installation target changed during copy");
				if (item.oldDirectory) { Move(target, Payload(transaction, L"backup", index), item.oldDirectory); Checkpoint("placed-old", index); }
				if (item.newDirectory) { Move(Payload(transaction, L"stage", index), target, item.newDirectory); Checkpoint("placed-new", index); }
			}
			// 全タイトルの配置後にだけ commit を記録する。ここより前の停止は全体を rollback する。
			Mark(transaction, L"commit", journal);
			Checkpoint("committed");
		}
		catch (...)
		{
			const auto failure = Exception();
			Recover(transaction);
			throw Failure(failure.error, failure.message.c_str());
		}
		Recover(transaction);
	}
	void Run(Request request)
	{
		std::lock_guard storageLock(storage);
		Result result;
		bool recoveryFailed = false;
		try
		{
			if (request.operation == Operation::Recover) RecoverAll();
			else Execute(request);
		}
		catch (...)
		{
			result = Exception();
			try { RecoverAll(); }
			catch (...) { result = Exception(); recoveryFailed = true; }
		}
		Snapshot current;
		try { if (!recoveryFailed) current = ReadInstalled(); }
		catch (...) { result = Exception(); recoveryFailed = true; }
		std::lock_guard lock(mutex);
		auto progress = snapshot.progress;
		progress.phase = Phase::Completed;
		progress.cancellable = false;
		progress.result = result;
		progress.completion = result ? Completion::Succeeded : result.error == Error::Cancelled ? Completion::Cancelled : Completion::Failed;
		if (result) progress.percent = 100;
		if (recoveryFailed) { current.state = State::RecoveryRequired; current.result = result; }
		current.progress = progress;
		snapshot = std::move(current);
		busy = false;
		finished.notify_all();
	}
};

GameInstallationService::GameInstallationService(const LauncherPaths& paths) : m_impl(std::make_unique<Impl>(paths)) {}
GameInstallationService::~GameInstallationService() { Cancel(); Wait(); }

GameInstallationService::Path GameInstallationService::TitlePath(const Path& data, uint64_t titleId)
{
	Describe(titleId);
	// TitleInfo::GetInstallPath() の user title 配置と一致する。
	return data / L"mlc01/usr/title" / Hex(titleId >> 32, 8) / Hex(static_cast<uint32_t>(titleId), 8);
}

GameInstallationService::Result GameInstallationService::Initialize()
{
	std::lock_guard lifecycleLock(m_impl->lifecycle);
	std::lock_guard storageLock(m_impl->storage);
	try
	{
		Require(!m_impl->initialized, Error::Busy, "Installer is already initialized");
		m_impl->paths.CheckPath(m_impl->paths.data);
		MakeDirectories(m_impl->transactions); MakeDirectories(m_impl->preparing);
		m_impl->anchors.Ancestors(m_impl->paths.data / L".launcher-install");
		m_impl->exclusive = Open(m_impl->paths.data / L".launcher-install/service.lock", GENERIC_READ | GENERIC_WRITE, OPEN_ALWAYS, 0);
		m_impl->anchors.Add(m_impl->transactions); m_impl->anchors.Add(m_impl->preparing);
		for (const auto high : s_titleHigh)
		{
			const auto parent = m_impl->paths.data / L"mlc01/usr/title" / Hex(high, 8);
			MakeDirectories(parent); m_impl->anchors.Ancestors(parent);
		}
		m_impl->RecoverAll();
		auto current = m_impl->ReadInstalled();
		std::lock_guard lock(m_impl->mutex);
		m_impl->snapshot = std::move(current);
		m_impl->initialized = true;
		return {};
	}
	catch (...)
	{
		const auto result = Impl::Exception();
		std::lock_guard lock(m_impl->mutex);
		m_impl->snapshot.state = State::RecoveryRequired;
		m_impl->snapshot.result = result;
		// 排他取得後の復旧失敗だけは、明示 Recover を受け付けられる。
		m_impl->initialized = m_impl->exclusive.value != INVALID_HANDLE_VALUE;
		return result;
	}
}

GameInstallationService::Analysis GameInstallationService::AnalyzeSource(const Path& selected) const
{
	std::lock_guard lock(m_impl->storage);
	return m_impl->Analyze(selected, nullptr);
}

GameInstallationService::Result GameInstallationService::Start(Request request)
{
	std::lock_guard lifecycleLock(m_impl->lifecycle);
	std::lock_guard lock(m_impl->mutex);
	if (!m_impl->initialized) return {Error::RecoveryRequired, "Initialize the installer before submitting operations"};
	if (m_impl->busy) return {Error::Busy, "Another installation operation is running"};
	if (m_impl->gameActive) return {Error::GameActive, "Stop the game before changing installed titles"};
	if (m_impl->snapshot.state == State::RecoveryRequired && request.operation != Operation::Recover) return {Error::RecoveryRequired, "Recover the previous installation transaction first"};
	if (request.operation < Operation::Install || request.operation > Operation::Recover) return {Error::InvalidMetadata, "Invalid installation operation"};
	if (m_impl->worker.joinable()) m_impl->worker.join();
	const auto previous = m_impl->snapshot;
	m_impl->busy = true;
	m_impl->cancelled = false;
	m_impl->snapshot.state = request.operation == Operation::ChangeRegion ? State::ChangingRegion : request.operation == Operation::Uninstall || request.operation == Operation::RemoveDLC ? State::Uninstalling :
		request.operation == Operation::ReinstallBase || request.operation == Operation::ReinstallUpdate ? State::Reinstalling : request.operation == Operation::Recover ? State::RecoveryRequired : State::Installing;
	m_impl->snapshot.ready = m_impl->snapshot.launchSupported = false;
	m_impl->snapshot.progress = {request.operation, Phase::Analyzing, 0, 0, 0, 0, request.operation != Operation::Recover, Completion::Running, {}};
	try { m_impl->worker = std::thread([impl = m_impl.get(), request = std::move(request)] { impl->Run(request); }); }
	catch (...) { m_impl->busy = false; m_impl->snapshot = previous; return Impl::Exception(); }
	return {};
}

void GameInstallationService::Cancel() { m_impl->cancelled = true; }
void GameInstallationService::Wait()
{
	std::thread completed;
	for (;;)
	{
		{
			std::unique_lock lock(m_impl->mutex);
			m_impl->finished.wait(lock, [&] { return !m_impl->busy; });
		}
		std::lock_guard lifecycleLock(m_impl->lifecycle);
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->busy) continue;
		completed = std::move(m_impl->worker);
		break;
	}
	if (completed.joinable()) completed.join();
}
GameInstallationService::Snapshot GameInstallationService::GetSnapshot() const { std::lock_guard lock(m_impl->mutex); return m_impl->snapshot; }
bool GameInstallationService::TryBeginPlay()
{
	std::lock_guard lock(m_impl->mutex);
	if (!m_impl->initialized || m_impl->busy || m_impl->gameActive || !m_impl->snapshot.launchSupported || m_impl->snapshot.state == State::RecoveryRequired) return false;
	m_impl->gameActive = true;
	return true;
}
void GameInstallationService::EndPlay() { std::lock_guard lock(m_impl->mutex); m_impl->gameActive = false; }
const char* GameInstallationService::StateName(State state)
{
	switch (state)
	{
	case State::NotInstalled: return "NotInstalled";
	case State::BaseOnly: return "BaseOnly";
	case State::UpdateMissing: return "UpdateMissing";
	case State::Ready: return "Ready";
	case State::Installing: return "Installing";
	case State::Reinstalling: return "Reinstalling";
	case State::ChangingRegion: return "ChangingRegion";
	case State::Uninstalling: return "Uninstalling";
	case State::RecoveryRequired: return "RecoveryRequired";
	case State::Error: return "Error";
	}
	return "Error";
}
#ifdef GAME_INSTALLATION_TESTING
void GameInstallationService::SetTestCheckpoint(std::function<void(std::string_view, uint64_t)> checkpoint) { s_checkpoint = std::move(checkpoint); }
#endif
