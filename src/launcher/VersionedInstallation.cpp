#include "VersionedInstallation.h"
#include "UpdateIdentity.h"
#include "LauncherPaths.h"
#include <glaze/glaze.hpp>
#include <zip.h>
#include <TlHelp32.h>
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <algorithm>
#include <set>

namespace LauncherUpdate
{
	namespace InstallationDetail
	{
		struct Marker
		{
			unsigned schema_version = 1;
			std::string product_id, app_id;
		};
		struct Foundation
		{
			unsigned schema_version = 1;
			std::string product_id, app_id, bootstrapper_version;
			unsigned updater_protocol_version = 2;
			std::vector<Manifest::Entry> files;
		};
	} // namespace InstallationDetail
	namespace
	{
		constexpr size_t MetadataLimit = 256 * 1024;
		std::wstring RegistrationKey()
		{
			return L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + Wide(AppId) + L"_is1";
		}
		std::wstring RegisteredPath()
		{
			std::array<wchar_t, 32768> value{};
			DWORD size = sizeof(value);
			const auto result = RegGetValueW(HKEY_CURRENT_USER, RegistrationKey().c_str(), L"InstallLocation", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, value.data(), &size);
			if (result == ERROR_FILE_NOT_FOUND)
				return {};
			Require(result == ERROR_SUCCESS && size > sizeof(wchar_t), "cannot-read-install-registration");
			auto path = fs::path(value.data());
			if (path.filename().empty())
				path = path.parent_path();
			return path.wstring();
		}
		void ValidateReference(const VersionRef& reference, bool empty = false)
		{
			if (empty && reference.version.empty() && reference.package_id.empty())
				return;
			Version::Parse(reference.version);
			Require(reference.package_id.size() == 64 && reference.package_id.find_first_not_of("0123456789abcdef") == std::string::npos, "invalid-version-identity");
		}
		template<class T>
		T ReadJson(const fs::path& path)
		{
			const auto bytes = ReadFileBytes(path, MetadataLimit);
			T result;
			Require(!glz::read<glz::opts{.error_on_missing_keys = true}>(result, bytes) && glz::write_json(result).value() == bytes, "invalid-installation-state");
			return result;
		}
		Manifest ReadVersionManifest(const fs::path& directory)
		{
			return VerifyManifest(ReadFileBytes(directory / L"manifest.json", MetadataLimit), ReadFileBytes(directory / L"manifest.sig", 128), TrustedPublicKey, ProductId);
		}
		InstallationDetail::Foundation ReadFoundation(const fs::path& root)
		{
			const auto bytes = ReadFileBytes(root / L"foundation.json", MetadataLimit);
			VerifySignature(bytes, ReadFileBytes(root / L"foundation.sig", 128), TrustedPublicKey);
			const auto record = ReadJson<InstallationDetail::Foundation>(root / L"foundation.json");
			Require(record.schema_version == 1 && record.product_id == ProductId && record.app_id == AppId && Version::Parse(record.bootstrapper_version) >= Version::Parse("1.0.0") && record.updater_protocol_version >= ProtocolVersion && record.files.size() == 2, "foundation-identity-or-protocol-mismatch");
			std::set<std::string> names;
			for (const auto& item : record.files)
			{
				Require((item.path == "MCWiiU-Launcher.exe" || item.path == "MCWiiU-Updater.exe") && names.insert(item.path).second && item.size > 0 && item.sha256.size() == 64 && item.sha256.find_first_not_of("0123456789abcdef") == std::string::npos, "unexpected-foundation-file");
			}
			return record;
		}
		void CheckTree(const fs::path& directory, const Manifest& manifest)
		{
			std::set<fs::path> files{L"manifest.json", L"manifest.sig"}, directories;
			for (const auto& entry : manifest.files)
			{
				const fs::path relative = Wide(entry.path);
				files.insert(relative);
				for (auto parent = relative.parent_path(); !parent.empty(); parent = parent.parent_path())
					directories.insert(parent);
			}
			for (const auto& entry : fs::recursive_directory_iterator(directory))
			{
				CheckOrdinaryPath(entry.path());
				const auto relative = entry.path().lexically_relative(directory);
				Require(entry.is_directory() ? directories.contains(relative) : files.contains(relative), "unknown-version-file-preserved");
			}
		}
		void CleanupCompletedCache(const fs::path& root)
		{
			const auto cache = LauncherPaths::LocalRoot() / L"Cache" / L"Updates";
			if (!fs::exists(cache))
				return;
			CheckOrdinaryPath(cache);
			PathLocks cacheLocks(cache);
			auto updater = OpenLocked(root / L"MCWiiU-Updater.exe", true);
			const auto updaterHash = FileSha256(updater.handle);
			unsigned inspected = 0;
			for (const auto& directory : fs::directory_iterator(cache))
			{
				if (++inspected > 1000)
					break;
				try
				{
					const auto id = Utf8(directory.path().filename().wstring());
					Require(id.size() == 64 && id.find_first_not_of("0123456789abcdef") == std::string::npos, "unknown-cache-directory");
					CheckOrdinaryPath(directory.path());
					const auto manifest = ReadVersionManifest(directory.path());
					Require(manifest.package_format == "zip", "retain-foundation-diagnostics");
					VerifyPackage(directory.path() / Wide(manifest.package_filename), manifest);
					std::set<fs::path> owned{L"manifest.json", L"manifest.sig", L"MCWiiU-Updater.exe", Wide(manifest.package_filename)};
					std::vector<File> locks;
					bool completed = false;
					unsigned count = 0;
					for (const auto& entry : fs::directory_iterator(directory.path()))
					{
						Require(++count <= 132, "cache-file-limit");
						const auto name = entry.path().filename();
						if (!owned.contains(name))
						{
							const auto stem = Utf8(name.stem().wstring());
							Require(name.extension() == L".state" && stem.size() == 64 && stem.find_first_not_of("0123456789abcdef") == std::string::npos, "unknown-cache-file-preserved");
							const auto state = ReadFileBytes(entry.path(), 128);
							Require(state == "AwaitingShutdown" || state == "Staging" || state == "Activating" || state == "VerifyingStartup" || state == "Committing" || state == "Completed", "retain-failure-diagnostics");
							completed = completed || state == "Completed";
							owned.insert(name);
						}
						locks.push_back(OpenLocked(entry.path()));
						if (name == L"MCWiiU-Updater.exe")
							Require(FileSha256(locks.back().handle) == updaterHash, "unverified-cached-updater-preserved");
					}
					Require(completed && count == owned.size(), "incomplete-cache-preserved");
					locks.clear();
					for (const auto& name : owned)
						Require(DeleteFileW((directory.path() / name).c_str()), "cache-in-use-preserved");
					Require(RemoveDirectoryW(directory.path().c_str()), "unknown-cache-content-preserved");
				} catch (const std::exception&)
				{
					OutputDebugStringA("Unverified, failed, or active update cache was retained.");
				}
			}
		}
		void RemoveVerifiedDirectory(const fs::path& root, const VersionRef& reference)
		{
			const auto directory = VersionDirectory(root, reference);
			auto verified = VerifyVersion(root, reference);
			CheckTree(directory, verified.manifest);
			verified.locks.clear();
			// 所有inventoryだけを削除し、未知ファイルや使用中ファイルは残す。
			std::set<fs::path, std::greater<>> directories;
			for (const auto& item : verified.manifest.files)
			{
				const auto file = directory / Wide(item.path);
				auto lock = OpenLocked(file);
				Require(FileSha256(lock.handle) == item.sha256, "version-changed-before-cleanup");
				lock = {};
				Require(DeleteFileW(file.c_str()), "version-in-use-or-cleanup-failed");
				for (auto parent = file.parent_path(); parent != directory; parent = parent.parent_path())
					directories.insert(parent);
			}
			for (const auto name : {L"manifest.json", L"manifest.sig"})
				Require(DeleteFileW((directory / name).c_str()), "version-cleanup-failed");
			for (const auto& path : directories)
				Require(RemoveDirectoryW(path.c_str()), "version-directory-not-empty");
			Require(RemoveDirectoryW(directory.c_str()), "version-directory-not-empty");
		}
	} // namespace
	fs::path ProcessImage(HANDLE process)
	{
		std::array<wchar_t, 32768> value{};
		DWORD size = static_cast<DWORD>(value.size());
		Require(QueryFullProcessImageNameW(process, 0, value.data(), &size), "cannot-identify-program");
		return fs::path(std::wstring(value.data(), size));
	}
	fs::path VersionDirectory(const fs::path& root, const VersionRef& reference)
	{
		ValidateReference(reference);
		const auto path = root / L"versions" / Wide(reference.version + "-" + reference.package_id.substr(0, 16));
		CheckOrdinaryPath(path);
		return path;
	}
	void ValidateRegisteredRoot(const fs::path& root, VerificationPurpose purpose)
	{
		ValidateInstallPath(root, LauncherPaths::LocalRoot());
		const auto marker = ReadJson<InstallationDetail::Marker>(root / L"installation.json");
		Require(marker.schema_version == 1 && marker.product_id == ProductId && marker.app_id == AppId && _wcsicmp(RegisteredPath().c_str(), root.c_str()) == 0, "installation-registration-mismatch");
		if (purpose == VerificationPurpose::Runtime)
			VerifyFoundationFiles(root);
		else
			ReadFoundation(root);
	}
	void VerifyFoundationFiles(const fs::path& root)
	{
		const auto record = ReadFoundation(root);
		for (const auto& item : record.files)
		{
			auto file = OpenLocked(root / Wide(item.path), true);
			LARGE_INTEGER size{};
			Require(GetFileSizeEx(file.handle, &size) && size.QuadPart >= 0 && static_cast<uint64_t>(size.QuadPart) == item.size && FileSha256(file.handle) == item.sha256, "foundation-file-integrity-failed");
			Require(ExecutableVersion(root / Wide(item.path)) == record.bootstrapper_version, "foundation-executable-version-mismatch");
		}
	}
	bool FoundationCompatible(const fs::path& root, const Manifest& manifest)
	{
		VerifyFoundationFiles(root);
		const auto record = ReadJson<InstallationDetail::Foundation>(root / L"foundation.json");
		return Version::Parse(manifest.required_bootstrapper_version) <= Version::Parse(record.bootstrapper_version) && manifest.required_updater_protocol_version <= ProtocolVersion && manifest.required_updater_protocol_version <= record.updater_protocol_version;
	}
	fs::path InstalledRoot(const fs::path& launcher)
	{
		Require(launcher.filename() == L"LauncherShell.exe" && launcher.parent_path().parent_path().filename() == L"versions", "updates-require-versioned-installation");
		const auto root = launcher.parent_path().parent_path().parent_path();
		ValidateRegisteredRoot(root);
		const auto current = ReadCurrent(root);
		auto verified = VerifyVersion(root, current.current);
		Require(verified.directory / L"LauncherShell.exe" == launcher, "launcher-is-not-current-version");
		return root;
	}
	void ValidateSetupDestination(const fs::path& root)
	{
		ValidateInstallPath(root, LauncherPaths::LocalRoot());
		const auto registered = RegisteredPath();
		Require(registered.empty() || _wcsicmp(registered.c_str(), root.c_str()) == 0, "product-already-installed-elsewhere");
		if (fs::exists(root) && !fs::is_empty(root))
		{
			// Setupは署名済み識別情報とLauncher全ファイルを確認してから、破損した基盤だけを置換できる。
			ValidateRegisteredRoot(root, VerificationPurpose::SetupRepair);
			if (!fs::exists(root / L"current.json") && !fs::exists(root / L"pending.json"))
			{
				Require(!fs::exists(root / L"versions") || fs::is_empty(root / L"versions"), "uninitialized-installation-requires-inspection");
			}
			else if (!fs::exists(root / L"current.json") && fs::exists(root / L"pending.json"))
			{
				const auto initial = ReadPending(root);
				Require(initial.stage == "Initial" && initial.previous.version.empty(), "manual-recovery-required");
				auto verified = VerifyVersion(root, initial.candidate, VerificationPurpose::SetupRepair);
			}
			else
			{
				auto current = ReadCurrent(root);
				auto verified = VerifyVersion(root, current.current, VerificationPurpose::SetupRepair);
				Require(!fs::exists(root / L"pending.json"), "finish-or-recover-pending-update-first");
			}
		}
		EnsureProgramsStopped(root);
	}
	void EnsureProgramsStopped(const fs::path& root)
	{
		File snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
		Require(snapshot.handle != INVALID_HANDLE_VALUE, "cannot-enumerate-programs");
		PROCESSENTRY32W entry{sizeof(entry)};
		if (Process32FirstW(snapshot.handle, &entry))
			do
			{
				if (entry.th32ProcessID == GetCurrentProcessId())
					continue;
				if (_wcsicmp(entry.szExeFile, L"LauncherShell.exe") && _wcsicmp(entry.szExeFile, L"MCWiiU-Launcher.exe") && _wcsicmp(entry.szExeFile, L"MCWiiU-Updater.exe") && _wcsicmp(entry.szExeFile, L"CemuBin.exe") && _wcsicmp(entry.szExeFile, L"Cemu_release.exe"))
					continue;
				File process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID));
				Require(process.handle, "cannot-identify-related-program");
				const auto image = ProcessImage(process.handle);
				const auto relative = image.lexically_relative(root);
				Require(relative.empty() || relative.begin()->wstring() == L"..", "program-still-running-close-normally");
			}
			while (Process32NextW(snapshot.handle, &entry));
	}
	File LockInstallation(const fs::path& root)
	{
		const auto path = root / L"operation.lock";
		CheckOrdinaryPath(path);
		File lock(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
		Require(lock.handle != INVALID_HANDLE_VALUE, "installation-operation-in-progress");
		BY_HANDLE_FILE_INFORMATION info{};
		Require(GetFileInformationByHandle(lock.handle, &info) && info.nNumberOfLinks == 1 && !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)), "invalid-installation-lock");
		return lock;
	}
	void AtomicWrite(const fs::path& path, std::string_view bytes)
	{
		const auto temporary = path.parent_path() / (path.filename().wstring() + L"." + Wide(RandomId()) + L".tmp");
		WriteNew(temporary, bytes);
		Require(ReadFileBytes(temporary, MetadataLimit) == bytes, "state-write-verification-failed");
		if (fs::exists(path))
		{
			auto existing = OpenLocked(path);
			existing = {};
			Require(ReplaceFileW(path.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr), "cannot-replace-state");
		}
		else
			Require(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH), "cannot-publish-state");
		Require(ReadFileBytes(path, MetadataLimit) == bytes, "state-replacement-verification-failed");
	}
	CurrentVersion ReadCurrent(const fs::path& root)
	{
		auto current = ReadJson<CurrentVersion>(root / L"current.json");
		Require(current.schema_version == 1 && current.product_id == ProductId, "invalid-current-identity");
		ValidateReference(current.current);
		ValidateReference(current.previous, true);
		Require(current.previous.version.empty() || Version::Parse(current.previous.version) < Version::Parse(current.current.version), "invalid-current-history");
		return current;
	}
	PendingVersion ReadPending(const fs::path& root)
	{
		auto pending = ReadJson<PendingVersion>(root / L"pending.json");
		Require(pending.schema_version == 1 && pending.updater_protocol_version == ProtocolVersion && pending.product_id == ProductId && pending.recovery_attempts <= 1, "invalid-pending-identity");
		Require(pending.transaction_id.size() == 64 && pending.transaction_id.find_first_not_of("0123456789abcdef") == std::string::npos, "invalid-transaction-id");
		ValidateReference(pending.candidate);
		ValidateReference(pending.previous, pending.stage == "Initial");
		ValidateReference(pending.prior_previous, true);
		Require(pending.stage == "Initial" || pending.stage == "Staged" || pending.stage == "Activating" || pending.stage == "VerifyingStartup" || pending.stage == "Committing" || pending.stage == "RollingBack" || pending.stage == "RecoveryRequired", "unknown-pending-stage");
		Require(pending.previous.version.empty() || Version::Parse(pending.previous.version) < Version::Parse(pending.candidate.version), "invalid-pending-version-order");
		Require(pending.prior_previous.version.empty() || (!pending.previous.version.empty() && Version::Parse(pending.prior_previous.version) < Version::Parse(pending.previous.version)), "invalid-pending-history");
		return pending;
	}
	void WritePending(const fs::path& root, const PendingVersion& pending)
	{
		AtomicWrite(root / L"pending.json", glz::write_json(pending).value());
	}
	void CommitVersion(const fs::path& root, const CurrentVersion& current)
	{
		Require(current.schema_version == 1 && current.product_id == ProductId, "invalid-current-identity");
		ValidateReference(current.current);
		ValidateReference(current.previous, true);
		Require(current.previous.version.empty() || Version::Parse(current.previous.version) < Version::Parse(current.current.version), "invalid-current-history");
		AtomicWrite(root / L"current.json", glz::write_json(current).value());
		Require(_wcsicmp(RegisteredPath().c_str(), root.c_str()) == 0, "registration-changed-before-version-display");
		HKEY key = nullptr;
		Require(RegOpenKeyExW(HKEY_CURRENT_USER, RegistrationKey().c_str(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS, "cannot-update-version-display");
		const auto version = Wide(current.current.version);
		const auto result = RegSetValueExW(key, L"DisplayVersion", 0, REG_SZ, reinterpret_cast<const BYTE*>(version.c_str()), static_cast<DWORD>((version.size() + 1) * sizeof(wchar_t)));
		RegCloseKey(key);
		Require(result == ERROR_SUCCESS, "cannot-update-version-display");
	}
	void ClearPending(const fs::path& root)
	{
		CheckOrdinaryPath(root / L"pending.json");
		Require(DeleteFileW((root / L"pending.json").c_str()), "cannot-complete-pending-update");
	}
	VerifiedVersion VerifyVersion(const fs::path& root, const VersionRef& reference, VerificationPurpose purpose)
	{
		VerifiedVersion result;
		result.reference = reference;
		result.directory = VersionDirectory(root, reference);
		const auto bytes = ReadFileBytes(result.directory / L"manifest.json", MetadataLimit);
		Require(Sha256(bytes) == reference.package_id, "version-package-id-mismatch");
		result.manifest = ReadVersionManifest(result.directory);
		const auto capabilities = ReadFoundation(root);
		const bool compatible = purpose == VerificationPurpose::Runtime ? FoundationCompatible(root, result.manifest) : Version::Parse(result.manifest.required_bootstrapper_version) <= Version::Parse(capabilities.bootstrapper_version) && result.manifest.required_updater_protocol_version <= ProtocolVersion && result.manifest.required_updater_protocol_version <= capabilities.updater_protocol_version;
		Require(result.manifest.version == reference.version && result.manifest.package_format == "zip" && compatible, "version-requires-foundation-update");
		CheckTree(result.directory, result.manifest);
		for (const auto& entry : result.manifest.files)
		{
			auto file = OpenLocked(result.directory / Wide(entry.path));
			LARGE_INTEGER size{};
			Require(GetFileSizeEx(file.handle, &size) && size.QuadPart >= 0 && static_cast<uint64_t>(size.QuadPart) == entry.size && FileSha256(file.handle) == entry.sha256, "installed-version-integrity-failed");
			result.locks.push_back(std::move(file));
		}
		Require(ExecutableVersion(result.directory / L"LauncherShell.exe") == reference.version, "version-executable-mismatch");
		return result;
	}
	VersionRef StageVersion(const fs::path& root, const fs::path& package, std::string_view bytes, std::string_view signature)
	{
		const auto manifest = VerifyManifest(bytes, signature, TrustedPublicKey, ProductId);
		Require(manifest.package_format == "zip", "not-a-version-package");
		Require(FoundationCompatible(root, manifest), "foundation-update-required");
		auto packageFile = OpenLocked(package);
		VerifyPackage(package, manifest);
		VersionRef reference{manifest.version, Sha256(bytes)};
		const auto destination = VersionDirectory(root, reference);
		if (fs::exists(destination))
		{
			auto verified = VerifyVersion(root, reference);
			return reference;
		}
		const auto versions = root / L"versions";
		if (!fs::exists(versions))
			CreatePrivateDirectory(versions);
		// 同じ表示versionで別内容を配布した場合も既存フォルダーへ追記しない。
		for (const auto& entry : fs::directory_iterator(versions))
			Require(!entry.path().filename().wstring().starts_with(Wide(reference.version + "-")), "conflicting-version-already-exists");
		uint64_t unpacked = 0;
		for (const auto& file : manifest.files)
			unpacked += file.size;
		ULARGE_INTEGER free{};
		Require(GetDiskFreeSpaceExW(root.c_str(), &free, nullptr, nullptr) && free.QuadPart >= unpacked + 64 * 1024 * 1024, "insufficient-update-disk-space");
		const auto staging = versions / (L"staging-" + Wide(RandomId()).substr(0, 16));
		for (const auto& entry : manifest.files) CheckOrdinaryPath(staging/Wide(entry.path));
		CreatePrivateDirectory(staging);
		PathLocks locks(versions);
		zip_error_t error; zip_error_init(&error);
		zip_source_t* source = zip_source_win32w_create(package.c_str(),0,-1,&error);
		zip_t* archive = source ? zip_open_from_source(source,ZIP_RDONLY | ZIP_CHECKCONS,&error) : nullptr;
		if (!archive && source) zip_source_free(source);
		zip_error_fini(&error);
		Require(archive, "invalid-zip-package");
		struct ZipOwner
		{
			zip_t* value;
			~ZipOwner()
			{
				zip_close(value);
			}
		} zipOwner{archive};
		const auto count = zip_get_num_entries(archive, ZIP_FL_UNCHANGED);
		Require(count == static_cast<zip_int64_t>(manifest.files.size()), "zip-inventory-count-mismatch");
		std::set<std::string> seen;
		for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(count); ++index)
		{
			zip_stat_t info{};
			constexpr zip_uint64_t required = ZIP_STAT_NAME | ZIP_STAT_SIZE | ZIP_STAT_COMP_METHOD | ZIP_STAT_ENCRYPTION_METHOD;
			Require(zip_stat_index(archive, index, ZIP_FL_ENC_STRICT, &info) == 0 && info.name && (info.valid & required) == required, "invalid-zip-entry");
			const std::string name(info.name);
			ValidatePackageName(name);
			auto folded = name;
			std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
			Require(seen.insert(folded).second, "duplicate-zip-entry");
			const auto found = std::find_if(manifest.files.begin(), manifest.files.end(), [&](const auto& item) { return item.path == name; });
			Require(found != manifest.files.end() && info.size == found->size && info.encryption_method == ZIP_EM_NONE && (info.comp_method == ZIP_CM_STORE || info.comp_method == ZIP_CM_DEFLATE), "zip-inventory-mismatch");
			zip_uint8_t host = 0;
			zip_uint32_t attributes = 0;
			Require(zip_file_get_external_attributes(archive, index, 0, &host, &attributes) == 0, "invalid-zip-attributes");
			const auto mode = (attributes >> 16) & 0170000;
			Require((mode == 0 || mode == 0100000) && !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)), "linked-or-special-zip-entry");
			const auto target = staging / Wide(name);
			if (!fs::exists(target.parent_path()))
				fs::create_directories(target.parent_path());
			CheckOrdinaryPath(target);
			zip_file_t* input = zip_fopen_index(archive, index, ZIP_FL_UNCHANGED);
			Require(input, "cannot-read-zip-entry");
			struct EntryOwner
			{
				zip_file_t* value;
				~EntryOwner()
				{
					zip_fclose(value);
				}
			} entryOwner{input};
			File output(CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
			Require(output.handle != INVALID_HANDLE_VALUE, "cannot-create-version-file");
			std::array<char, 65536> buffer{};
			uint64_t total = 0;
			for (;;)
			{
				const auto read = zip_fread(input, buffer.data(), buffer.size());
				Require(read >= 0 && total + read <= found->size, "zip-data-invalid");
				if (!read)
					break;
				DWORD written = 0;
				Require(WriteFile(output.handle, buffer.data(), static_cast<DWORD>(read), &written, nullptr) && written == read, "zip-write-failed");
				total += read;
			}
			Require(total == found->size && FlushFileBuffers(output.handle), "zip-size-or-flush-failed");
			output = {};
			auto check = OpenLocked(target);
			Require(FileSha256(check.handle) == found->sha256, "zip-file-hash-mismatch");
		}
		WriteNew(staging / L"manifest.json", bytes);
		WriteNew(staging / L"manifest.sig", signature);
		CheckTree(staging, manifest);
		Require(ExecutableVersion(staging / L"LauncherShell.exe") == manifest.version, "staged-executable-version-mismatch");
		Require(MoveFileExW(staging.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH), "cannot-publish-version-directory");
		auto verified = VerifyVersion(root, reference);
		return reference;
	}
	void CleanupVersions(const fs::path& root, const CurrentVersion& current)
	{
		Require(!fs::exists(root / L"pending.json"), "cleanup-blocked-by-pending-update");
		for (const auto& entry : fs::directory_iterator(root / L"versions"))
		{
			if (entry.path() == VersionDirectory(root, current.current) || (!current.previous.version.empty() && entry.path() == VersionDirectory(root, current.previous)))
				continue;
			CheckOrdinaryPath(entry.path());
			Require(entry.is_directory(), "unknown-version-entry-preserved");
			const auto manifest = ReadVersionManifest(entry.path());
			VersionRef reference{manifest.version, Sha256(ReadFileBytes(entry.path() / L"manifest.json", MetadataLimit))};
			Require(VersionDirectory(root, reference) == entry.path(), "unknown-version-directory-preserved");
			EnsureProgramsStopped(entry.path());
			RemoveVerifiedDirectory(root, reference);
		}
	}
	void InitializeInstallation(const fs::path& root, const fs::path& source)
	{
		ValidateInstallPath(root, LauncherPaths::LocalRoot());
		EnsureProgramsStopped(root);
		if (!fs::exists(root / L"installation.json"))
			WriteNew(root / L"installation.json", glz::write_json(InstallationDetail::Marker{1, std::string(ProductId), std::string(AppId)}).value());
		ValidateRegisteredRoot(root);
		if (fs::exists(root / L"current.json"))
		{
			const auto current = ReadCurrent(root);
			auto verified = VerifyVersion(root, current.current);
			CommitVersion(root, current);
			return;
		}
		const auto bytes = ReadFileBytes(source / L"manifest.json", MetadataLimit), signature = ReadFileBytes(source / L"manifest.sig", 128);
		const auto manifest = VerifyManifest(bytes, signature, TrustedPublicKey, ProductId);
		const auto reference = StageVersion(root, source / Wide(manifest.package_filename), bytes, signature);
		if (fs::exists(root / L"pending.json"))
		{
			const auto initial = ReadPending(root);
			Require(initial.stage == "Initial" && initial.previous.version.empty() && initial.candidate == reference, "initialization-conflict-use-matching-setup");
		}
		PendingVersion pending;
		pending.product_id = ProductId;
		pending.transaction_id = RandomId();
		pending.stage = "Initial";
		pending.candidate = reference;
		WritePending(root, pending);
	}
	void RemoveOwnedVersions(const fs::path& root)
	{
		ValidateInstallPath(root, LauncherPaths::LocalRoot());
		const auto marker = ReadJson<InstallationDetail::Marker>(root / L"installation.json");
		Require(marker.product_id == ProductId && marker.app_id == AppId, "uninstall-identity-mismatch");
		EnsureProgramsStopped(root);
		auto operation = LockInstallation(root);
		PathLocks pathLocks(root);
		LARGE_INTEGER lockSize{};
		Require(GetFileSizeEx(operation.handle, &lockSize) && lockSize.QuadPart == 0, "unknown-operation-lock-preserved");
		if (fs::exists(root / L"pending.json"))
		{
			ReadPending(root);
			throw std::runtime_error("recover-pending-update-before-uninstall");
		}
		const auto current = ReadCurrent(root);
		auto check = VerifyVersion(root, current.current);
		check.locks.clear();
		// 未知の追加内容があれば一切のversion削除に着手しない。
		std::vector<VersionRef> owned;
		for (const auto& entry : fs::directory_iterator(root / L"versions"))
		{
			const auto manifest = ReadVersionManifest(entry.path());
			VersionRef reference{manifest.version, Sha256(ReadFileBytes(entry.path() / L"manifest.json", MetadataLimit))};
			Require(VersionDirectory(root, reference) == entry.path(), "unknown-version-directory-preserved");
			auto verified = VerifyVersion(root, reference);
			CheckTree(entry.path(), verified.manifest);
			owned.push_back(reference);
		}
		for (const auto& reference : owned)
			RemoveVerifiedDirectory(root, reference);
		try
		{
			CleanupCompletedCache(root);
		} catch (const std::exception&)
		{
			OutputDebugStringA("Update cache retained for inspection.");
		}
		Require(RemoveDirectoryW((root / L"versions").c_str()), "versions-directory-not-empty");
		for (const auto name : {L"current.json", L"installation.json"})
			Require(DeleteFileW((root / name).c_str()), "cannot-remove-owned-state");
		operation = {};
		Require(DeleteFileW((root / L"operation.lock").c_str()), "cannot-remove-owned-operation-lock");
	}
	void VerifyFoundationSetup(const fs::path& setup, const Manifest& manifest)
	{
		Require(manifest.package_format == "inno-setup", "not-a-foundation-package");
		VerifyPackage(setup, manifest);
		Require(ExecutableProductName(setup) == ProductName && ExecutableVersion(setup) == manifest.required_bootstrapper_version, "foundation-setup-product-mismatch");
		Require(!TrustedSetupPublisher.empty(), "trusted-setup-publisher-not-configured");
		WINTRUST_FILE_INFO file{sizeof(file)};
		file.pcwszFilePath = setup.c_str();
		WINTRUST_DATA trust{sizeof(trust)};
		trust.dwUIChoice = WTD_UI_NONE;
		trust.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
		trust.dwUnionChoice = WTD_CHOICE_FILE;
		trust.pFile = &file;
		trust.dwStateAction = WTD_STATEACTION_VERIFY;
		GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
		const auto result = WinVerifyTrust(nullptr, &action, &trust);
		bool publisher = false;
		if (result == ERROR_SUCCESS)
		{
			const auto* provider = WTHelperProvDataFromStateData(trust.hWVTStateData);
			auto* signer = provider ? WTHelperGetProvSignerFromChain(const_cast<CRYPT_PROVIDER_DATA*>(provider), 0, FALSE, 0) : nullptr;
			if (signer && signer->csCertChain)
			{
				std::array<BYTE, 32> hash{};
				DWORD size = static_cast<DWORD>(hash.size());
				if (CertGetCertificateContextProperty(signer->pasCertChain[0].pCert, CERT_SHA256_HASH_PROP_ID, hash.data(), &size))
				{
					std::string text;
					constexpr char hex[] = "0123456789abcdef";
					for (auto byte : hash)
					{
						text += hex[byte >> 4];
						text += hex[byte & 15];
					}
					publisher = text == TrustedSetupPublisher;
				}
			}
		}
		trust.dwStateAction = WTD_STATEACTION_CLOSE;
		WinVerifyTrust(nullptr, &action, &trust);
		Require(result == ERROR_SUCCESS && publisher, "foundation-authenticode-publisher-mismatch");
	}
} // namespace LauncherUpdate
