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
	void CopyNew(const fs::path& source, const fs::path& destination, const Cancel& cancel)
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
	}
	void CopyTree(const fs::path& source, const fs::path& destination, const Cancel& cancel)
	{
		Ordinary(source);
		Require(fs::is_directory(source), "Required Mii initialization input directory is missing");
		for (const auto& entry : fs::recursive_directory_iterator(source))
		{
			CheckCancel(cancel);
			Ordinary(entry.path());
			if (entry.is_regular_file()) CopyNew(entry.path(), destination / entry.path().lexically_relative(source), cancel);
			else Require(entry.is_directory(), "Unsupported Mii initialization input");
		}
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
}

struct MiiInitializationStorage::Impl
{
	fs::path data, mlc, root, working;
	Journal journal;
	Handle service;
	std::vector<Handle> originals;
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
				CopyNew(generated.files[i].path, staged, {});
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

void MiiInitializationStorage::Recover(const fs::path& data)
{
	if (!fs::exists(data / L".mii-initialization/journal.dat")) return;
	Impl state(data);
	if (!state.Load() || state.journal.phase == Phase::Complete || state.journal.phase == Phase::Abandoned) return;
	if (state.journal.phase == Phase::Ready) state.Finish({});
	else
	{
		// Preparingでは正式DBを作らない。未知の変更は保護し、自動削除もしない。
		state.PreserveOriginals(false);
		state.journal.phase = Phase::Abandoned;
		state.Save();
	}
}

MiiInitializationStorage::MiiInitializationStorage(const fs::path& data, const std::vector<fs::path>& titles, Cancel cancel)
	: m_impl(std::make_unique<Impl>(data))
{
	auto& state = *m_impl;
	if (state.Load()) Require(state.journal.phase == Phase::Complete || state.journal.phase == Phase::Abandoned, "Pending Mii initialization recovery");
	state.journal = {};
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
		CopyTree(title, state.working / relative, cancel);
	}
	CopyTree(state.mlc / L"sys/title/0005001b/10056000", state.working / L"sys/title/0005001b/10056000", cancel);
	CopyTree(state.mlc / L"usr/save/system/act", state.working / L"usr/save/system/act", cancel);
	fs::create_directories(NativePath(Database(state.working)));
	for (size_t i = 0; i < s_names.size(); ++i)
		if (state.journal.present[i]) CopyNew(inspection.files[i].path, Database(state.working) / s_names[i], cancel);
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
	state.journal.phase = Phase::Ready;
	state.Save();
	state.Finish(cancel);
}
