#include "UpdateHandoff.h"
#include "UpdateIdentity.h"
#include "VersionedInstallation.h"
#include "LauncherPaths.h"
#include <TlHelp32.h>
#include <shellapi.h>
#include <glaze/glaze.hpp>
#include <array>
#include <chrono>
#include <thread>

namespace LauncherUpdate
{
	namespace
	{
		fs::path Image(HANDLE process)
		{
			std::array<wchar_t, 32768> buffer{};
			DWORD size = static_cast<DWORD>(buffer.size());
			Require(QueryFullProcessImageNameW(process, 0, buffer.data(), &size), "cannot-identify-update-process");
			return fs::path(std::wstring(buffer.data(), size));
		}
		void CheckParent(HANDLE process)
		{
			File snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
			PROCESSENTRY32W entry{sizeof(entry)};
			bool matched = false;
			if (Process32FirstW(snapshot.handle, &entry))
				do
				{
					if (entry.th32ProcessID == GetCurrentProcessId())
					{
						matched = entry.th32ParentProcessID == GetProcessId(process);
						break;
					}
				}
				while (Process32NextW(snapshot.handle, &entry));
			Require(matched && WaitForSingleObject(process, 0) == WAIT_TIMEOUT, "unauthenticated-update-parent");
		}
		struct Pipes
		{
			File read, write;
			Pipes(bool inheritRead)
			{
				SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
				HANDLE r = nullptr, w = nullptr;
				Require(CreatePipe(&r, &w, &security, 65536), "cannot-create-update-channel");
				read = File(r);
				write = File(w);
				Require(SetHandleInformation(inheritRead ? w : r, HANDLE_FLAG_INHERIT, 0), "cannot-protect-update-channel");
			}
		};
		File InheritableSelf()
		{
			HANDLE handle = nullptr;
			Require(DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &handle, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0), "cannot-authenticate-update-parent");
			return File(handle);
		}
		PROCESS_INFORMATION Spawn(const fs::path& executable, std::wstring arguments, std::vector<HANDLE> inherited)
		{
			SIZE_T size = 0;
			InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
			std::vector<unsigned char> buffer(size);
			auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
			Require(InitializeProcThreadAttributeList(list, 1, 0, &size), "cannot-build-update-process");
			const bool configured = UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(), inherited.size() * sizeof(HANDLE), nullptr, nullptr) != FALSE;
			STARTUPINFOEXW startup{};
			startup.StartupInfo.cb = sizeof(startup);
			startup.lpAttributeList = list;
			PROCESS_INFORMATION process{};
			auto command = L"\"" + executable.wstring() + L"\" " + arguments;
			const bool created = configured && CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT, nullptr, executable.parent_path().c_str(), &startup.StartupInfo, &process);
			DeleteProcThreadAttributeList(list);
			Require(created, "cannot-start-update-process");
			return process;
		}
		std::wstring HandleText(HANDLE handle)
		{
			return std::to_wstring(reinterpret_cast<uintptr_t>(handle));
		}
		void Send(HANDLE pipe, std::string_view bytes)
		{
			Require(bytes.size() <= 1024 * 1024, "update-channel-limit");
			DWORD written = 0;
			const auto length = static_cast<DWORD>(bytes.size());
			Require(WriteFile(pipe, &length, sizeof(length), &written, nullptr) && written == sizeof(length), "update-channel-write-failed");
			Require(WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size(), "update-channel-write-failed");
		}
		std::string Receive(HANDLE pipe, HANDLE peer, unsigned seconds)
		{
			const auto deadline = GetTickCount64() + seconds * 1000ULL;
			std::string bytes;
			DWORD length = 0;
			bool framed = false;
			while (GetTickCount64() < deadline)
			{
				DWORD available = 0;
				Require(PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr), "update-channel-closed");
				if (available)
				{
					DWORD read = 0;
					if (!framed && available >= sizeof(length))
					{
						Require(ReadFile(pipe, &length, sizeof(length), &read, nullptr) && read == sizeof(length) && length > 0 && length <= 1024 * 1024, "invalid-update-channel-frame");
						framed = true;
						bytes.reserve(length);
					}
					else if (framed)
					{
						const auto count = std::min<DWORD>(available, length - static_cast<DWORD>(bytes.size()));
						const auto offset = bytes.size();
						bytes.resize(offset + count);
						Require(ReadFile(pipe, bytes.data() + offset, count, &read, nullptr) && read == count, "update-channel-read-failed");
						if (bytes.size() == length)
							return bytes;
					}
					continue;
				}
				Require(WaitForSingleObject(peer, 0) == WAIT_TIMEOUT, "update-peer-exited");
				std::this_thread::sleep_for(std::chrono::milliseconds(25));
			}
			throw std::runtime_error("update-handshake-timeout");
		}
		void Log(const fs::path& directory, std::string_view state)
		{
			// 固定した状態コードだけを保存し、URL/nonce/秘密情報を記録しない。
			WriteNew(directory / (Wide(RandomId()) + L".state"), state);
		}
		BOOL CALLBACK RequestChildClose(HWND window, LPARAM process)
		{
			DWORD owner = 0;
			GetWindowThreadProcessId(window, &owner);
			if (owner == static_cast<DWORD>(process) && IsWindowVisible(window) && !GetWindow(window, GW_OWNER))
				PostMessageW(window, WM_CLOSE, 0, 0);
			return TRUE;
		}
		bool LaunchReady(const fs::path& executable, std::string_view version, File* launched = nullptr, DWORD* launchedPid = nullptr, bool transient = false)
		{
			Pipes ready(false);
			auto parent = InheritableSelf();
			const auto nonce = RandomId();
			const auto process = Spawn(executable, L"--update-ready " + HandleText(ready.write.handle) + L" " + HandleText(parent.handle) + L" " + Wide(nonce), {ready.write.handle, parent.handle});
			File owner(process.hProcess), thread(process.hThread);
			ready.write = File{};
			bool healthy = false;
			try
			{
				healthy = Receive(ready.read.handle, owner.handle, 30) == nonce + ":" + std::to_string(process.dwProcessId) + ":" + std::string(version);
				if (healthy && transient)
				{
					DWORD exit = 1;
					healthy = WaitForSingleObject(owner.handle, 5000) == WAIT_OBJECT_0 && GetExitCodeProcess(owner.handle, &exit) && exit == 0;
				}
				else if (healthy)
					healthy = WaitForSingleObject(owner.handle, 1000) == WAIT_TIMEOUT;
			} catch (const std::exception&)
			{}
			if (!healthy && WaitForSingleObject(owner.handle, 0) == WAIT_TIMEOUT)
			{
				// 起動した子だけに通常の終了を依頼する。終了しなければ復旧を止め、強制終了しない。
				EnumWindows(RequestChildClose, static_cast<LPARAM>(process.dwProcessId));
				WaitForSingleObject(owner.handle, 5000);
			}
			if (healthy && launched)
				*launched = std::move(owner);
			if (healthy && launchedPid)
				*launchedPid = process.dwProcessId;
			return healthy;
		}

		bool ActivateExistingLauncher(const fs::path& executable)
		{
			File snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
			Require(snapshot.handle != INVALID_HANDLE_VALUE, "cannot-enumerate-launchers");
			PROCESSENTRY32W entry{sizeof(entry)};
			if (Process32FirstW(snapshot.handle, &entry))
				do
				{
					if (_wcsicmp(entry.szExeFile, L"LauncherShell.exe"))
						continue;
					File process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID));
					Require(process.handle, "cannot-identify-launcher");
					if (Image(process.handle) != executable)
						continue;
					STARTUPINFOW startup{sizeof(startup)};
					PROCESS_INFORMATION secondary{};
					auto command = L"\"" + executable.wstring() + L"\"";
					Require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, executable.parent_path().c_str(), &startup, &secondary), "cannot-activate-existing-launcher");
					File owner(secondary.hProcess), thread(secondary.hThread);
					DWORD exit = 1;
					Require(WaitForSingleObject(owner.handle, 10000) == WAIT_OBJECT_0 && GetExitCodeProcess(owner.handle, &exit) && exit == 0, "existing-launcher-activation-failed");
					return true;
				}
				while (Process32NextW(snapshot.handle, &entry));
			return false;
		}
		void SaveStage(const fs::path& root, PendingVersion& pending, std::string_view stage)
		{
			pending.stage = stage;
			WritePending(root, pending);
		}
		void Recover(const fs::path& root, PendingVersion& pending)
		{
			Require(!pending.previous.version.empty() && pending.recovery_attempts == 0, "automatic-recovery-exhausted-use-verified-setup");
			auto previous = VerifyVersion(root, pending.previous);
			EnsureProgramsStopped(root);
			const auto updates = LauncherPaths::LocalRoot() / L"Cache" / L"Updates";
			CheckOrdinaryPath(updates);
			if (!fs::exists(updates))
				CreatePrivateDirectory(updates);
			const auto evidence = updates / Wide(pending.transaction_id);
			if (!fs::exists(evidence))
				CreatePrivateDirectory(evidence);
			pending.recovery_attempts = 1;
			SaveStage(root, pending, "RollingBack");
			Log(evidence, "RecoveryAttempt:1");
			CommitVersion(root, CurrentVersion{1, std::string(ProductId), pending.previous, pending.prior_previous});
			Require(LaunchReady(previous.directory / L"LauncherShell.exe", previous.reference.version), "previous-version-startup-failed-manual-recovery-required");
			Log(evidence, "VerifiedPreviousReady");
			ClearPending(root);
		}
	} // namespace
	File StartUpdater(const fs::path& transaction, const Handoff& handoff)
	{
		const auto root = InstalledRoot(Image(GetCurrentProcess()));
		const auto source = root / L"MCWiiU-Updater.exe";
		auto locked = OpenLocked(source);
		const auto expected = FileSha256(locked.handle);
		const auto destination = transaction / L"MCWiiU-Updater.exe";
		Require(CopyFileW(source.c_str(), destination.c_str(), TRUE), "cannot-stage-updater");
		auto staged = OpenLocked(destination);
		Require(FileSha256(staged.handle) == expected, "staged-updater-hash-mismatch");
		Pipes input(true), acknowledgement(false);
		auto parent = InheritableSelf();
		const auto process = Spawn(destination, L"--handoff " + HandleText(input.read.handle) + L" " + HandleText(parent.handle) + L" " + HandleText(acknowledgement.write.handle), {input.read.handle, parent.handle, acknowledgement.write.handle});
		File owner(process.hProcess), thread(process.hThread);
		input.read = {};
		acknowledgement.write = {};
		Send(input.write.handle, glz::write_json(handoff).value());
		Require(Receive(acknowledgement.read.handle, owner.handle, 15) == handoff.nonce + ":accepted", "updater-handoff-rejected");
		return owner;
	}
	bool RunUpdater(HANDLE input, HANDLE parent, HANDLE acknowledgement, const std::function<void(State)>& status, std::atomic_uint* applyGate)
	{
		fs::path transaction, root;
		try
		{
			CheckParent(parent);
			const auto launcher = Image(parent);
			root = InstalledRoot(launcher);
			transaction = Image(GetCurrentProcess()).parent_path();
			CheckOrdinaryPath(transaction);
			Require(transaction.parent_path() == LauncherPaths::LocalRoot() / L"Cache" / L"Updates" && transaction.filename().wstring().size() == 64, "invalid-update-transaction-directory");
			PathLocks cacheLocks(transaction), installLocks(root);
			auto operation = LockInstallation(root);
			Handoff handoff;
			Require(!glz::read<glz::opts{.error_on_missing_keys = true}>(handoff, Receive(input, parent, 15)), "invalid-update-handoff");
			Require(handoff.protocol_version == ProtocolVersion, "unsupported-handoff-protocol");
			Candidate candidate;
			candidate.bytes = handoff.manifest;
			candidate.signature = handoff.signature;
			candidate.manifest = VerifyManifest(candidate.bytes, candidate.signature, TrustedPublicKey, ProductId);
			BindRelease(candidate, handoff.release);
			Require(candidate.targetId == handoff.targetId && handoff.nonce.size() == 64 && handoff.nonce.find_first_not_of("0123456789abcdef") == std::string::npos, "stale-update-handoff");
			Require(!fs::exists(root / L"pending.json"), "recover-existing-transaction-first");
			const auto current = ReadCurrent(root);
			auto previous = VerifyVersion(root, current.current);
			Require(previous.directory / L"LauncherShell.exe" == launcher && RollbackCompatible(previous.manifest, candidate.manifest), "stale-or-data-incompatible-update");
			const auto package = transaction / Wide(candidate.manifest.package_filename);
			auto packageLock = OpenLocked(package);
			VerifyPackage(package, candidate.manifest);
			if (candidate.manifest.package_format == "zip")
				Require(FoundationCompatible(root, candidate.manifest), "foundation-update-required");
			else
				VerifyFoundationSetup(package, candidate.manifest);
			if (status)
				status(State::AwaitingShutdown);
			Log(transaction, "AwaitingShutdown");
			Send(acknowledgement, handoff.nonce + ":accepted");
			Require(WaitForSingleObject(parent, 120000) == WAIT_OBJECT_0, "launcher-normal-exit-timeout");
			DWORD exit = 1;
			Require(GetExitCodeProcess(parent, &exit) && exit == 0, "launcher-exit-failed");
			EnsureProgramsStopped(root);
			const auto rechecked = ReadCurrent(root);
			Require(rechecked.current == current.current && rechecked.previous == current.previous, "installation-changed-during-handoff");
			unsigned expected = 0;
			if (applyGate && !applyGate->compare_exchange_strong(expected, 2))
			{
				Require(LaunchReady(launcher, current.current.version), "cancelled-update-restart-failed");
				Log(transaction, "CancelledBeforeActivation");
				return true;
			}
			if (candidate.manifest.package_format == "inno-setup")
			{
				if (status) status(State::Activating);
				Log(transaction,"FoundationInstalling");
				VerifyFoundationSetup(package, candidate.manifest);
				previous.locks.clear();
				// Setup自身による基盤更新だけを許可する。Launcher更新の承認は引き継がない。
				auto command = L"\"" + package.wstring() + L"\" /CURRENTUSER /NORESTART /NOLAUNCH=1 /NOCLOSEAPPLICATIONS /NORESTARTAPPLICATIONS /DIR=\"" + root.wstring() + L"\"";
				STARTUPINFOW startup{sizeof(startup)};
				PROCESS_INFORMATION process{};
				Require(CreateProcessW(package.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, transaction.c_str(), &startup, &process), "cannot-start-foundation-setup");
				File owner(process.hProcess), thread(process.hThread);
				packageLock = {};
				Require(WaitForSingleObject(owner.handle, INFINITE) == WAIT_OBJECT_0 && GetExitCodeProcess(owner.handle, &exit) && exit == 0, "foundation-setup-failed-use-verified-setup");
				ValidateRegisteredRoot(root);
				Require(FoundationCompatible(root, candidate.manifest), "foundation-setup-did-not-provide-required-version");
				for (const auto& item : candidate.manifest.files)
				{
					auto file = OpenLocked(root / Wide(item.path), true);
					LARGE_INTEGER size{};
					Require(GetFileSizeEx(file.handle, &size) && size.QuadPart >= 0 && static_cast<uint64_t>(size.QuadPart) == item.size && FileSha256(file.handle) == item.sha256, "foundation-result-inventory-mismatch");
				}
				const auto retained = ReadCurrent(root);
				Require(retained.current == current.current && retained.previous == current.previous, "foundation-changed-launcher-state");
				auto verified = VerifyVersion(root, current.current);
				operation = {};
				Require(LaunchReady(root / L"MCWiiU-Launcher.exe", current.current.version, nullptr, nullptr, true), "foundation-bootstrapper-restart-failed");
				Log(transaction, "FoundationCompleted");
				return true;
			}
			if (status)
				status(State::Staging);
			Log(transaction, "Staging");
			const auto next = StageVersion(root, package, candidate.bytes, candidate.signature);
			PendingVersion pending;
			pending.product_id = ProductId;
			pending.transaction_id = Utf8(transaction.filename().wstring());
			pending.stage = "Staged";
			pending.candidate = next;
			pending.previous = current.current;
			pending.prior_previous = current.previous;
			WritePending(root, pending);
			File newProcess;
			DWORD newPid = 0;
			try
			{
				if (status)
					status(State::Activating);
				SaveStage(root, pending, "Activating");
				Log(transaction, "Activating");
				auto verified = VerifyVersion(root, next);
				SaveStage(root, pending, "VerifyingStartup");
				if (status)
					status(State::VerifyingStartup);
				Log(transaction, "VerifyingStartup");
				Require(LaunchReady(verified.directory / L"LauncherShell.exe", next.version, &newProcess, &newPid), "new-version-startup-failed");
				SaveStage(root, pending, "Committing");
				if (status)
					status(State::Committing);
				Log(transaction, "Committing");
				CommitVersion(root, CurrentVersion{1, std::string(ProductId), next, current.current});
				ClearPending(root);
				Log(transaction, "Completed");
			} catch (const std::exception& error)
			{
				Log(transaction,std::string("ActivationFailed:")+error.what());
				if (newProcess.handle != INVALID_HANDLE_VALUE && WaitForSingleObject(newProcess.handle, 0) == WAIT_TIMEOUT)
				{
					EnumWindows(RequestChildClose, static_cast<LPARAM>(newPid));
					WaitForSingleObject(newProcess.handle, 5000);
				}
				EnsureProgramsStopped(root);
				if (status)
					status(State::RollingBack);
				Log(transaction, "RollingBack");
				Recover(root, pending);
				Log(transaction, "RestoredPreviousVersion");
				return true;
			}
			// 正常版は削除対象から外す。未知・使用中の旧版はログを残して保持する。
			try
			{
				CleanupVersions(root, ReadCurrent(root));
			} catch (const std::exception& error)
			{
				Log(transaction, std::string("CleanupDeferred:") + error.what());
			}
			return true;
		} catch (const std::exception& error)
		{
			if (status) status(State::RecoveryRequired);
			if (!transaction.empty())
				try
				{
					Log(transaction, std::string("RecoveryRequired:") + error.what());
				} catch (...)
				{}
			MessageBoxW(nullptr, (L"Update stopped safely. Keep the update logs and use a verified Setup for manual repair. User data was not rolled back.\n\n" + Wide(error.what())).c_str(), L"MCWiiU Updater", MB_OK | MB_ICONERROR);
			return false;
		}
	}
	bool RunBootstrapper(std::wstring_view arguments)
	{
		try
		{
			const auto root = Image(GetCurrentProcess()).parent_path();
			ValidateRegisteredRoot(root);
			auto operation = LockInstallation(root);
			PathLocks locks(root);
			const auto finish = [&] {
				const auto actual = ReadCurrent(root);
				NotifyUpdateReady(arguments, actual.current.version);
				return true;
			};
			if (fs::exists(root / L"pending.json"))
			{
				auto pending = ReadPending(root);
				if (pending.stage == "Initial")
				{
					Require(pending.recovery_attempts == 0 && !fs::exists(root / L"current.json"), "initial-startup-already-attempted");
					auto next = VerifyVersion(root, pending.candidate);
					EnsureProgramsStopped(root);
					pending.recovery_attempts = 1;
					WritePending(root, pending);
					Require(LaunchReady(next.directory / L"LauncherShell.exe", next.reference.version), "initial-launcher-startup-failed-use-verified-setup");
					CommitVersion(root, CurrentVersion{1, std::string(ProductId), pending.candidate, {}});
					ClearPending(root);
					return finish();
				}
				const auto current = ReadCurrent(root);
				Require((current.current == pending.previous && current.previous == pending.prior_previous) || (pending.stage == "Committing" && current.current == pending.candidate && current.previous == pending.previous), "current-pending-conflict");
				Recover(root, pending);
				return finish();
			}
			const auto current = ReadCurrent(root);
			auto verified = VerifyVersion(root, current.current);
			if (ActivateExistingLauncher(verified.directory / L"LauncherShell.exe"))
				return finish();
			EnsureProgramsStopped(root);
			if (LaunchReady(verified.directory / L"LauncherShell.exe", current.current.version))
				return finish();
			EnsureProgramsStopped(root);
			Require(!current.previous.version.empty(), "no-verified-previous-version");
			PendingVersion pending;
			pending.product_id = ProductId;
			pending.transaction_id = RandomId();
			pending.stage = "RollingBack";
			pending.candidate = current.current;
			pending.previous = current.previous;
			WritePending(root, pending);
			Recover(root, pending);
			return finish();
		} catch (const std::exception& error)
		{
			MessageBoxW(nullptr, (L"Launcher startup requires recovery. Use a verified Setup; keep current/pending state and existing user data.\n\n" + Wide(error.what())).c_str(), L"MCWiiU Launcher", MB_OK | MB_ICONERROR);
			return false;
		}
	}
	bool NotifyUpdateReady(std::wstring_view arguments, std::string_view version)
	{
		if (!arguments.starts_with(L"--update-ready "))
			return false;
		int count = 0;
		const auto command = L"LauncherShell.exe " + std::wstring(arguments);
		auto** argv = CommandLineToArgvW(command.c_str(), &count);
		Require(argv && count == 5, "invalid-ready-channel");
		const auto pipe = reinterpret_cast<HANDLE>(_wcstoui64(argv[2], nullptr, 10)), parent = reinterpret_cast<HANDLE>(_wcstoui64(argv[3], nullptr, 10));
		const auto nonce = Utf8(argv[4]);
		LocalFree(argv);
		File channel(pipe), peer(parent);
		CheckParent(parent);
		const auto parentImage = Image(parent);
		const auto ownImage = Image(GetCurrentProcess());
		Require(nonce.size() == 64 && nonce.find_first_not_of("0123456789abcdef") == std::string::npos &&
					(parentImage.filename() == L"MCWiiU-Updater.exe" || (parentImage.filename() == L"MCWiiU-Launcher.exe" && parentImage.parent_path() == ownImage.parent_path().parent_path().parent_path())),
				"invalid-ready-parent");
		Send(pipe, nonce + ":" + std::to_string(GetCurrentProcessId()) + ":" + (version.empty() ? std::string(LAUNCHER_VERSION) : std::string(version)));
		return true;
	}
} // namespace LauncherUpdate
