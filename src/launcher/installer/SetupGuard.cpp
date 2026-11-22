#include "../VersionedInstallation.h"
#include "../UpdateIdentity.h"
#include "../LauncherPaths.h"
#include <msi.h>
#include <TlHelp32.h>
#include <stdexcept>

namespace
{
	LauncherUpdate::File s_operation;
	std::filesystem::path s_operationRoot;
	void ReserveSetup(const std::filesystem::path& root)
	{
		if (!std::filesystem::exists(root / L"installation.json"))
			return;
		if (s_operation.handle != INVALID_HANDLE_VALUE)
		{
			LauncherUpdate::Require(s_operationRoot == root, "setup-reservation-mismatch");
			return;
		}
		s_operation = LauncherUpdate::LockInstallation(root);
		s_operationRoot = root;
	}
	bool LegacyMsiPresent()
	{
		std::array<wchar_t,39> legacyCode{};
		const auto legacy = MsiEnumRelatedProductsW(L"{5CCFE313-2AD6-4C18-9115-FBC53873B134}",0,0,legacyCode.data());
		if (legacy == ERROR_SUCCESS) return true;
		LauncherUpdate::Require(legacy == ERROR_NO_MORE_ITEMS,"cannot-audit-legacy-msi-family");
		for (DWORD index = 0; index < 1024; ++index)
		{
			std::array<wchar_t, 39> code{};
			std::array<wchar_t, 256> sid{};
			DWORD size = static_cast<DWORD>(sid.size());
			MSIINSTALLCONTEXT context{};
			const auto result = MsiEnumProductsExW(nullptr, nullptr, MSIINSTALLCONTEXT_ALL, index, code.data(), &context, sid.data(), &size);
			if (result == ERROR_NO_MORE_ITEMS)
				return false;
			LauncherUpdate::Require(result == ERROR_SUCCESS, "cannot-audit-legacy-msi-registration");
			std::array<wchar_t, 2048> name{};
			size = static_cast<DWORD>(name.size());
			const auto query = MsiGetProductInfoExW(code.data(), context == MSIINSTALLCONTEXT_MACHINE ? nullptr : sid.data(), context, L"ProductName", name.data(), &size);
			LauncherUpdate::Require(query == ERROR_SUCCESS, "cannot-identify-legacy-msi-product");
			if (std::wstring_view(name.data()) == L"MCWiiU Launcher" || LauncherUpdate::Utf8(name.data()) == LauncherUpdate::ProductId)
				return true;
		}
		throw std::runtime_error("legacy-msi-audit-limit");
	}
	template<class Operation>
	int Guard(Operation operation)
	{
		try
		{
			operation();
			return 0;
		} catch (const std::exception& error)
		{
			s_operation = {};
			OutputDebugStringA(error.what());
			return 1;
		}
	}
} // namespace
extern "C" __declspec(dllexport) int __stdcall ValidateDestination(const wchar_t* path)
{
	return Guard([&] { LauncherUpdate::Require(path && *path,"missing-install-path"); LauncherUpdate::Require(!LegacyMsiPresent(),"existing-msi-requires-manual-migration"); LauncherUpdate::ValidateSetupDestination(path); ReserveSetup(path); });
}
extern "C" __declspec(dllexport) int __stdcall FinishInstallation(const wchar_t* path, const wchar_t* source)
{
	return Guard([&] { LauncherUpdate::Require(path && source,"missing-install-source"); LauncherUpdate::InitializeInstallation(path,source); s_operation = {}; });
}
extern "C" __declspec(dllexport) int __stdcall ValidateBundle(const wchar_t* source)
{
	return Guard([&] {
		using namespace LauncherUpdate; Require(source && *source,"missing-install-source"); const std::filesystem::path directory(source);
		const auto manifest = VerifyManifest(ReadFileBytes(directory/L"manifest.json",256*1024),ReadFileBytes(directory/L"manifest.sig",128),TrustedPublicKey,ProductId);
		Require(manifest.package_format == "zip","invalid-initial-package-format"); VerifyPackage(directory/Wide(manifest.package_filename),manifest);
		const auto foundation = VerifyFoundationMetadata(ReadFileBytes(directory/L"foundation.json",256*1024), ReadFileBytes(directory/L"foundation.sig",128), TrustedPublicKey);
		Require(Version::Parse(foundation.bootstrapper_version) <= Version::Parse(BootstrapperVersion) && Version::Parse(manifest.required_bootstrapper_version) <= Version::Parse(foundation.bootstrapper_version) && manifest.required_updater_protocol_version <= foundation.updater_protocol_version, "setup-foundation-incompatible");
		for (const auto& entry : foundation.files) {
			auto file = OpenLocked(directory/Wide(entry.path)); LARGE_INTEGER size{};
			Require(GetFileSizeEx(file.handle,&size) && size.QuadPart >= 0 && static_cast<uint64_t>(size.QuadPart) == entry.size && FileSha256(file.handle) == entry.sha256 && ExecutableVersion(directory/Wide(entry.path)) == foundation.bootstrapper_version, "setup-foundation-integrity-failed");
		}
	});
}
extern "C" __declspec(dllexport) int __stdcall PrepareRemoval(const wchar_t* path)
{
	return Guard([&] {
		using namespace LauncherUpdate; const std::filesystem::path root(path);
		ValidateRegisteredRoot(root); EnsureProgramsStopped(root); Require(!std::filesystem::exists(root/L"pending.json"),"recover-update-before-uninstall");
		auto operation = LockInstallation(root); const auto current = ReadCurrent(root); auto active = VerifyVersion(root,current.current); active.locks.clear(); unsigned count = 0;
		// InitializeUninstallの段階で全所有情報を検査し、標準削除の開始前に停止できるようにする。
		for (const auto& entry : std::filesystem::directory_iterator(root/L"versions")) {
			Require(++count <= 128,"uninstall-version-limit"); const auto bytes = ReadFileBytes(entry.path()/L"manifest.json",256*1024);
			const auto manifest = VerifyManifest(bytes,ReadFileBytes(entry.path()/L"manifest.sig",128),TrustedPublicKey,ProductId); const VersionRef reference{manifest.version,Sha256(bytes)};
			Require(VersionDirectory(root,reference) == entry.path(),"unknown-version-directory-preserved"); auto verified = VerifyVersion(root,reference); verified.locks.clear();
			for (const auto& file : manifest.files) { const auto target = entry.path()/Wide(file.path); File check(CreateFileW(target.c_str(),GENERIC_READ | DELETE,FILE_SHARE_READ | FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)); Require(check.handle != INVALID_HANDLE_VALUE,"version-in-use-preserved"); }
		}
	});
}
extern "C" __declspec(dllexport) int __stdcall RemoveVersions(const wchar_t* path)
{
	return Guard([&] { LauncherUpdate::RemoveOwnedVersions(path); });
}
