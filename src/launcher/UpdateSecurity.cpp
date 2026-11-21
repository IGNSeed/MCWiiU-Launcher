#include "UpdateSecurity.h"
#include "UpdateIdentity.h"

#include <Aclapi.h>
#include <ShlObj.h>
#include <sddl.h>
#include <winhttp.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <glaze/glaze.hpp>
#include <algorithm>
#include <charconv>
#include <memory>
#include <stdexcept>
#include <cwctype>
#include <utility>
#include <format>

namespace LauncherUpdate
{
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::wstring Wide(std::string_view value)
{
	if (value.empty()) return {};
	const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
	Require(size > 0, "invalid-utf8");
	std::wstring result(size, 0);
	Require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size) == size, "invalid-utf8");
	return result;
}
std::string Utf8(std::wstring_view value)
{
	if (value.empty()) return {};
	const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	Require(size > 0, "invalid-unicode");
	std::string result(size, 0);
	Require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) == size, "invalid-unicode");
	return result;
}
Version Version::Parse(std::string_view text)
{
	Version result;
	unsigned* parts[] = {&result.major, &result.minor, &result.patch};
	for (unsigned index = 0; index < 3; ++index)
	{
		const auto dot = text.find('.');
		const auto part = text.substr(0, dot);
		Require(!part.empty() && part.size() <= 5 && (part.size() == 1 || part.front() != '0'), "invalid-version");
		const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), *parts[index]);
		Require(error == std::errc{} && end == part.data() + part.size() && *parts[index] <= 65535U, "invalid-version");
		Require(index == 2 ? dot == text.npos : dot != text.npos, "invalid-version");
		if (index < 2) text.remove_prefix(dot + 1);
	}
	return result;
}
std::string_view Name(State state)
{
 constexpr std::string_view names[] = {"Idle","Checking","UpToDate","UpdateAvailable","Scheduled","Downloading","Verifying","Staging","Staged","AwaitingShutdown","Activating","VerifyingStartup","Committing","Completed","RollingBack","Failed","RecoveryRequired"};
 Require(static_cast<unsigned>(state) < std::size(names), "invalid-update-state");
 return names[static_cast<unsigned>(state)];
}
bool CanCancel(State state) { return state == State::Checking || (state >= State::UpdateAvailable && state <= State::Staged); }
bool RollbackCompatible(const Manifest& previous, const Manifest& next)
{
 return previous.product_id == next.product_id && previous.architecture == next.architecture &&
  Version::Parse(previous.version) < Version::Parse(next.version) && previous.compatibility.rollback_safe && next.compatibility.rollback_safe && previous.compatibility == next.compatibility;
}
bool CanTransition(State from, State to)
{
 if (to == State::Failed || to == State::RecoveryRequired) return from != State::Completed;
 if (to == State::Idle) return CanCancel(from) || from == State::UpToDate || from == State::Failed;
 switch (from) {
 case State::Idle: case State::UpToDate: case State::Failed: case State::UpdateAvailable: return to == State::Checking || (from == State::UpdateAvailable && to == State::Scheduled);
 case State::Checking: return to == State::UpToDate || to == State::UpdateAvailable;
 case State::Scheduled: return to == State::Downloading;
 case State::Downloading: return to == State::Verifying;
 case State::Verifying: return to == State::Staging;
 case State::Staging: return to == State::Staged;
 case State::Staged: return to == State::AwaitingShutdown;
 case State::AwaitingShutdown: return to == State::Activating;
 case State::Activating: return to == State::VerifyingStartup || to == State::RollingBack;
 case State::VerifyingStartup: return to == State::Committing || to == State::RollingBack;
 case State::Committing: return to == State::Completed;
 default: return false;
 }
}

namespace
{
	std::string Hex(const unsigned char* bytes, size_t size)
	{
		constexpr char digits[] = "0123456789abcdef"; std::string result;
		for (size_t i = 0; i < size; ++i) { result += digits[bytes[i] >> 4]; result += digits[bytes[i] & 15]; }
		return result;
	}
	std::vector<unsigned char> Unhex(std::string_view text, size_t size)
	{
		Require(text.size() == size * 2 && text.find_first_not_of("0123456789abcdef") == text.npos, "invalid-hex");
		std::vector<unsigned char> result(size);
		for (size_t i = 0; i < size; ++i) { unsigned value = 0; std::from_chars(text.data() + 2 * i, text.data() + 2 * i + 2, value, 16); result[i] = static_cast<unsigned char>(value); }
		return result;
	}
	std::wstring UserSid()
	{
		HANDLE raw = nullptr; Require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw), "user-token-unavailable"); File token(raw);
		DWORD size = 0; GetTokenInformation(raw, TokenUser, nullptr, 0, &size); std::vector<unsigned char> bytes(size);
		Require(GetTokenInformation(raw, TokenUser, bytes.data(), size, &size), "user-token-unavailable");
		PWSTR text = nullptr; Require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &text), "invalid-user-sid");
		std::wstring result(text); LocalFree(text); return result;
	}
	bool Below(const fs::path& path, const fs::path& base)
	{
		auto p = path.wstring(), b = base.wstring();
		std::transform(p.begin(), p.end(), p.begin(), towlower); std::transform(b.begin(), b.end(), b.begin(), towlower);
		return p == b || (p.starts_with(b) && p.size() > b.size() && p[b.size()] == L'\\');
	}
	void CheckAcl(const fs::path& path)
	{
		PSECURITY_DESCRIPTOR descriptor = nullptr; PACL acl = nullptr;
		Require(GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &descriptor) == ERROR_SUCCESS, "cannot-inspect-install-acl");
		std::unique_ptr<void, decltype(&LocalFree)> owned(descriptor, LocalFree);
		Require(acl != nullptr, "unrestricted-install-acl");
		const auto user = UserSid();
		for (DWORD index = 0; index < acl->AceCount; ++index)
		{
			void* raw = nullptr; Require(GetAce(acl, index, &raw), "invalid-install-acl");
			const auto header = static_cast<ACE_HEADER*>(raw);
			Require(header->AceType != ACCESS_ALLOWED_OBJECT_ACE_TYPE && header->AceType != ACCESS_ALLOWED_CALLBACK_ACE_TYPE && header->AceType != ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE, "unsupported-install-acl");
			if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERIT_ONLY_ACE)) continue;
			const auto ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
			constexpr DWORD writes = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER | FILE_DELETE_CHILD | GENERIC_WRITE | GENERIC_ALL;
			if (!(ace->Mask & writes)) continue;
			PWSTR sid = nullptr; Require(ConvertSidToStringSidW(&ace->SidStart, &sid), "invalid-install-acl");
			const std::wstring identity(sid); LocalFree(sid);
			Require(identity == user || identity == L"S-1-5-18" || identity == L"S-1-5-32-544" || identity == L"S-1-3-0", "install-path-writable-by-other-users");
		}
	}
}
std::string Sha256(std::string_view bytes)
{
	std::array<unsigned char, EVP_MAX_MD_SIZE> hash{}; unsigned size = 0;
	Require(EVP_Digest(bytes.data(), bytes.size(), hash.data(), &size, EVP_sha256(), nullptr) == 1, "hash-failed"); return Hex(hash.data(), size);
}
std::string FileSha256(HANDLE file)
{
	LARGE_INTEGER zero{}; Require(SetFilePointerEx(file, zero, nullptr, FILE_BEGIN), "file-seek-failed");
	std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
	Require(context && EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) == 1, "hash-failed");
	std::array<char, 65536> buffer{}; DWORD size = 0;
	for (;;) { Require(ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &size, nullptr), "file-read-failed"); if (!size) break; Require(EVP_DigestUpdate(context.get(), buffer.data(), size) == 1, "hash-failed"); }
	std::array<unsigned char, EVP_MAX_MD_SIZE> hash{}; unsigned length = 0;
	Require(EVP_DigestFinal_ex(context.get(), hash.data(), &length) == 1, "hash-failed"); return Hex(hash.data(), length);
}
std::string RandomId() { std::array<unsigned char, 32> bytes{}; Require(RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) == 1, "random-failed"); return Hex(bytes.data(), bytes.size()); }
void VerifySignature(std::string_view bytes, std::string_view signature, std::string_view key)
{
	Require(!key.empty(), "trusted-update-key-not-configured");
	Require(!bytes.empty() && bytes.size() <= 256 * 1024 && bytes.find('\0') == bytes.npos, "invalid-manifest-size");
	const auto publicKey = Unhex(key, 32), sig = Unhex(signature, 64);
	std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> verifier(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, publicKey.data(), publicKey.size()), EVP_PKEY_free);
	std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
	Require(verifier && context && EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, verifier.get()) == 1 && EVP_DigestVerify(context.get(), sig.data(), sig.size(), reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()) == 1, "invalid-manifest-signature");
}
Manifest VerifyManifest(std::string_view bytes, std::string_view signature, std::string_view key, std::string_view product)
{
	VerifySignature(bytes,signature,key);
	Manifest manifest;
	Require(!glz::read<glz::opts{.error_on_missing_keys = true}>(manifest, bytes), "invalid-manifest-json");
	// 署名対象は UTF-8、BOM/改行なし、宣言順の compact JSON。重複キーや別表現も拒否する。
	Require(glz::write_json(manifest).value() == bytes, "noncanonical-manifest");
	const auto version = Version::Parse(manifest.version); (void)version;
 Require(manifest.schema_version == 2 && manifest.product_id == product && manifest.channel == "stable" && manifest.architecture == "x64", "incompatible-manifest");
 Require(manifest.release_tag == "v" + manifest.version, "manifest-product-mismatch");
 Version::Parse(manifest.required_bootstrapper_version);
 Require(manifest.required_updater_protocol_version > 0, "invalid-protocol-version");
 Require(manifest.package_format == "zip" || manifest.package_format == "inno-setup", "invalid-package-format");
 const auto extension = manifest.package_format == "zip" ? "-win-x64.zip" : "-setup-x64.exe";
 Require(manifest.package_filename == std::string(product) + "-" + manifest.version + extension && manifest.release_asset_id > 0 && manifest.release_asset_id <= 9007199254740991ULL && manifest.package_size > 0 && manifest.package_size <= 512ULL * 1024 * 1024, "invalid-package-asset");
 Unhex(manifest.package_sha256, 32);
 Require(!manifest.files.empty() && manifest.files.size() <= 1024, "invalid-file-inventory");
 std::vector<std::string> names; uint64_t total = 0; unsigned launchers = 0;
 for (const auto& file : manifest.files) {
  ValidatePackageName(file.path); Unhex(file.sha256, 32);
  auto name = file.path; std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
  Require(std::find(names.begin(), names.end(), name) == names.end(), "duplicate-package-file"); names.push_back(name);
  Require(file.size <= 512ULL * 1024 * 1024 && total <= 2ULL * 1024 * 1024 * 1024 - file.size, "unpacked-size-limit"); total += file.size;
  if (file.path == "LauncherShell.exe") ++launchers;
  if (manifest.package_format == "zip") {
   Require(file.path == "LauncherShell.exe" || file.path == "WebView2Loader.dll" || file.path.starts_with("licenses/"), "unexpected-package-file");
   if (file.path.starts_with("licenses/")) for (const auto extension : {".exe",".dll",".com",".bat",".cmd",".ps1",".vbs",".js",".msi",".scr",".lnk"}) Require(!name.ends_with(extension),"executable-in-license-directory");
  }
 }
 Require(manifest.package_format != "zip" || launchers == 1, "missing-package-launcher");
	if (manifest.package_format == "inno-setup") {
		Require(manifest.files.size() == 2, "invalid-foundation-inventory");
		for (const auto& file : manifest.files) Require(file.path == "MCWiiU-Launcher.exe" || file.path == "MCWiiU-Updater.exe", "unexpected-foundation-file");
	}
	const auto& c = manifest.compatibility;
	Require(c.preferences_schema == 2 && c.history_schema == 1 && c.account_schema == 1 && c.game_data_schema == 1, "incompatible-data-schema");
	return manifest;
}
void BindRelease(Candidate& candidate, std::string_view json)
{
	const auto parsed = glz::read_json<glz::generic>(json); Require(bool(parsed), "invalid-release-json");
	const auto& release = parsed.value();
	try
	{
		Require(release["draft"].get<bool>() == false && release["prerelease"].get<bool>() == false && release["tag_name"].get<std::string>() == candidate.manifest.release_tag, "not-a-stable-release");
		const auto id = release["id"].get<double>(); Require(id > 0 && id <= 9007199254740991.0 && id == static_cast<uint64_t>(id), "invalid-release-id"); candidate.releaseId = static_cast<uint64_t>(id);
		candidate.notes = release["body"].is_null() ? "" : release["body"].get<std::string>(); Require(candidate.notes.size() <= 65536, "release-notes-too-large");
		unsigned matched = 0;
		for (const auto& asset : release["assets"].get<glz::generic::array_t>())
		{
			if (asset["id"].get<double>() != candidate.manifest.release_asset_id) continue;
			++matched;
			Require(asset["name"].get<std::string>() == candidate.manifest.package_filename && asset["size"].get<double>() == candidate.manifest.package_size && asset["state"].get<std::string>() == "uploaded", "release-asset-mismatch");
			candidate.url = asset["browser_download_url"].get<std::string>();
			Require(candidate.url == "https://github.com/IGNSeed/MCWiiU-Launcher/releases/download/" + candidate.manifest.release_tag + "/" + candidate.manifest.package_filename, "invalid-release-asset-url");
			const auto& object = asset.get<glz::generic::object_t>(); const auto digest = object.find("digest");
			if (digest != object.end() && !digest->second.is_null()) Require(digest->second.get<std::string>() == "sha256:" + candidate.manifest.package_sha256, "github-digest-mismatch");
		}
		Require(matched == 1, "missing-or-duplicate-package-asset");
		candidate.targetId = Sha256(std::to_string(candidate.releaseId) + ":" + candidate.bytes + ":" + candidate.signature);
	} catch (const std::bad_variant_access&) { throw std::runtime_error("invalid-release-json"); }
}
File::~File() { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
File::File(File&& other) noexcept : handle(std::exchange(other.handle, INVALID_HANDLE_VALUE)) {}
File& File::operator=(File&& other) noexcept { if (this != &other) { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); handle = std::exchange(other.handle, INVALID_HANDLE_VALUE); } return *this; }
void CheckOrdinaryPath(const fs::path& path)
{
	Require(path.is_absolute() && path == path.lexically_normal() && path.wstring().size() < 240 && path.wstring().find(L':', 2) == std::wstring::npos && path.wstring().find_first_of(L"\"<>|?*") == std::wstring::npos && !path.wstring().starts_with(L"\\\\"), "invalid-file-path");
	for (auto current = path; !current.empty(); current = current.parent_path())
	{
		const auto name = current.filename().wstring(); Require(name.empty() || (name.back() != L'.' && name.back() != L' '), "ambiguous-file-path");
		const auto attributes = GetFileAttributesW(current.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES) Require(GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND, "path-access-denied");
		else Require(!(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "reparse-point-rejected");
		if (current == current.root_path()) break;
	}
}
File OpenLocked(const fs::path& path, bool executable)
{
	CheckOrdinaryPath(path);
	File file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | (executable ? FILE_SHARE_DELETE : 0), nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
	BY_HANDLE_FILE_INFORMATION info{};
	Require(file.handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(file.handle, &info) && info.nNumberOfLinks == 1 && !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)), "unsafe-or-locked-file");
	return file;
}
PathLocks::PathLocks(const fs::path& path)
{
	CheckOrdinaryPath(path);
	for (auto current = path; !current.empty(); current = current.parent_path())
	{
		File directory(CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
		BY_HANDLE_FILE_INFORMATION info{};
		Require(directory.handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(directory.handle, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "cannot-lock-update-directory");
		m_directories.push_back(std::move(directory));
		if (current == current.root_path()) break;
	}
}
std::string ReadFileBytes(const fs::path& path, size_t limit)
{
	auto file = OpenLocked(path); LARGE_INTEGER size{};
	Require(GetFileSizeEx(file.handle, &size) && size.QuadPart >= 0 && static_cast<uint64_t>(size.QuadPart) <= limit, "file-size-limit");
	std::string bytes(static_cast<size_t>(size.QuadPart), 0); DWORD read = 0;
	Require(ReadFile(file.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size(), "file-read-failed"); return bytes;
}
void WriteNew(const fs::path& path, std::string_view bytes)
{
	CheckOrdinaryPath(path); File file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)); DWORD written = 0;
	Require(file.handle != INVALID_HANDLE_VALUE && WriteFile(file.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(file.handle), "file-write-failed");
}
void CreatePrivateDirectory(const fs::path& path)
{
	CheckOrdinaryPath(path);
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	const auto sddl = L"D:P(A;OICI;FA;;;" + UserSid() + L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
	Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr), "cannot-build-cache-acl");
	std::unique_ptr<void, decltype(&LocalFree)> owned(descriptor, LocalFree); SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
	Require(CreateDirectoryW(path.c_str(), &attributes), "cannot-create-private-cache"); CheckAcl(path);
}
void ValidateInstallPath(const fs::path& path, const fs::path& dataRoot)
{
	CheckOrdinaryPath(path); Require(path != path.root_path() && !Below(path, dataRoot) && !Below(dataRoot, path), "install-overlaps-user-data");
	Require(GetDriveTypeW(path.root_path().c_str()) == DRIVE_FIXED, "install-requires-local-fixed-drive");
	std::array<wchar_t, MAX_PATH> windows{}; Require(GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size())), "cannot-find-windows");
	Require(!Below(path, windows.data()), "system-install-path-rejected");
	for (const auto* folder : {&FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86, &FOLDERID_ProgramData})
	{
		PWSTR raw = nullptr; const auto result = SHGetKnownFolderPath(*folder, 0, nullptr, &raw);
		if (SUCCEEDED(result) && raw) { const bool forbidden = Below(path, raw); CoTaskMemFree(raw); Require(!forbidden, "protected-install-path-rejected"); }
	}
	// 既存の全親を監査する。ドライブルートの子だけでは共有フォルダーを判定できない。
	for (auto current = path; current != current.root_path(); current = current.parent_path())
		if (fs::exists(current)) CheckAcl(current);
	auto parent = path; while (!fs::exists(parent)) parent = parent.parent_path();
	const auto probe = parent / (L".mcwiiu-access-" + Wide(RandomId()));
	File file(CreateFileW(probe.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW, FILE_FLAG_DELETE_ON_CLOSE | FILE_ATTRIBUTE_TEMPORARY, nullptr));
	Require(file.handle != INVALID_HANDLE_VALUE, "install-path-not-writable");
}
std::string ExecutableVersion(const fs::path& executable)
{
	CheckOrdinaryPath(executable); DWORD unused = 0;
	const auto length = GetFileVersionInfoSizeW(executable.c_str(), &unused); Require(length && length <= 1024 * 1024, "cannot-read-executable-version");
	std::vector<unsigned char> data(length); Require(GetFileVersionInfoW(executable.c_str(), 0, length, data.data()), "cannot-read-executable-version");
	VS_FIXEDFILEINFO* info = nullptr; UINT size = 0;
	Require(VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &size) && size >= sizeof(*info) && info->dwSignature == 0xfeef04bd && LOWORD(info->dwProductVersionLS) == 0, "invalid-executable-version");
	return std::to_string(HIWORD(info->dwProductVersionMS)) + "." + std::to_string(LOWORD(info->dwProductVersionMS)) + "." + std::to_string(HIWORD(info->dwProductVersionLS));
}
std::string ExecutableProductName(const fs::path& executable)
{
	CheckOrdinaryPath(executable); DWORD unused = 0; const auto length = GetFileVersionInfoSizeW(executable.c_str(),&unused);
	Require(length && length <= 1024*1024,"cannot-read-executable-product"); std::vector<unsigned char> data(length);
	Require(GetFileVersionInfoW(executable.c_str(),0,length,data.data()),"cannot-read-executable-product");
	struct Translation { WORD language, codePage; }; Translation* translation = nullptr; UINT size = 0;
	Require(VerQueryValueW(data.data(),L"\\VarFileInfo\\Translation",reinterpret_cast<void**>(&translation),&size) && size >= sizeof(Translation),"missing-executable-language");
	const auto query = std::format(L"\\StringFileInfo\\{:04x}{:04x}\\ProductName",translation->language,translation->codePage); wchar_t* value = nullptr;
	Require(VerQueryValueW(data.data(),query.c_str(),reinterpret_cast<void**>(&value),&size) && size > 1 && size <= 256 && value[size-1] == L'\0',"missing-executable-product");
	auto name = std::wstring_view(value,size-1);
	// Inno Setupのversion resourceには固定長の末尾空白が付く。署名とhashは元bytesのまま検証する。
	while (!name.empty() && name.back() == L' ') name.remove_suffix(1);
	Require(!name.empty(),"missing-executable-product");
	return Utf8(name);
}
bool AllowedUrl(std::string_view url, bool cdn)
{
	if (url.size() > 8192 || url.find_first_of("\r\n\\\t\" ") != url.npos || url.find('#') != url.npos || url.find('@') != url.npos) return false;
	if (url == "https://api.github.com/repos/IGNSeed/MCWiiU-Launcher/releases/latest" || url.starts_with("https://github.com/IGNSeed/MCWiiU-Launcher/releases/download/v")) return url.find("..") == url.npos;
	return cdn && url.starts_with("https://release-assets.githubusercontent.com/github-production-release-asset/");
}
HttpResponse Fetch(std::string url, size_t limit, std::atomic_bool& cancel, HANDLE output, Progress progress)
{
	Require(AllowedUrl(url), "untrusted-update-url");
	const bool asset = url.starts_with("https://github.com/");
	struct Http { HINTERNET value; ~Http() { if (value) WinHttpCloseHandle(value); } };
	Http session{WinHttpOpen(L"MCWiiU-Launcher/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0)};
	Require(session.value && WinHttpSetTimeouts(session.value, 5000, 5000, 10000, 10000), "http-initialization-failed");
	const auto deadline = GetTickCount64() + (output == INVALID_HANDLE_VALUE ? 30000 : 600000);
	for (unsigned redirects = 0; redirects <= 3; ++redirects)
	{
		Require(!cancel, "cancelled"); Require(GetTickCount64() < deadline, "http-total-timeout");
		const auto wide = Wide(url); URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts); parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
		Require(WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) && parts.nScheme == INTERNET_SCHEME_HTTPS && parts.nPort == 443, "invalid-https-url");
		const std::wstring host(parts.lpszHostName, parts.dwHostNameLength), path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) + std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);
		Http connection{WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)};
		Http request{connection.value ? WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr};
		DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
		Require(request.value && WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)), "http-request-failed");
		Require(WinHttpSendRequest(request.value, L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n", static_cast<DWORD>(-1), nullptr, 0, 0, 0) && WinHttpReceiveResponse(request.value, nullptr), "http-network-or-timeout");
		DWORD status = 0, statusSize = sizeof(status);
		Require(WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &statusSize, nullptr), "invalid-http-status");
		if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
		{
			std::array<wchar_t, 8192> location{}; DWORD size = static_cast<DWORD>(location.size() * sizeof(wchar_t));
			Require(asset && redirects < 3 && WinHttpQueryHeaders(request.value, WINHTTP_QUERY_LOCATION, nullptr, location.data(), &size, nullptr), "http-redirect-rejected");
			url = Utf8(location.data()); Require(AllowedUrl(url, true), "untrusted-http-redirect"); continue;
		}
		if (status == 404 && !asset) return {404, {}};
		Require(status == 200, "github-api-or-http-error");
		std::array<wchar_t, 64> length{}; DWORD lengthSize = static_cast<DWORD>(length.size() * sizeof(wchar_t)); std::optional<uint64_t> contentLength;
		if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, length.data(), &lengthSize, nullptr))
		{
			const auto text = Utf8(length.data()); uint64_t value = 0; const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
			Require(!text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value <= limit, "invalid-content-length"); contentLength = value;
		}
		else Require(GetLastError() == ERROR_WINHTTP_HEADER_NOT_FOUND, "invalid-content-length");
		HttpResponse response{status, {}}; uint64_t total = 0; std::array<char, 65536> buffer{};
		for (;;)
		{
			Require(!cancel, "cancelled"); Require(GetTickCount64() < deadline, "http-total-timeout"); DWORD read = 0;
			Require(WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read), "incomplete-http-response"); if (!read) break;
			total += read; Require(total <= limit, "http-size-limit");
			if (output == INVALID_HANDLE_VALUE) response.body.append(buffer.data(), read);
			else { DWORD written = 0; Require(WriteFile(output, buffer.data(), read, &written, nullptr) && written == read, "download-write-failed"); }
			if (progress) progress(total);
		}
		Require(!cancel && (!contentLength || total == *contentLength), "incomplete-http-response"); return response;
	}
	throw std::runtime_error("too-many-redirects");
}
void ValidatePackageName(std::string_view name)
{
 Require(!name.empty() && name.size() <= 160 && name.front() != '/' && name.back() != '/', "invalid-package-path");
 Require(name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./-") == name.npos, "invalid-package-path");
 size_t offset = 0;
 while (offset < name.size()) {
  const auto end = name.find('/', offset); auto part = name.substr(offset, end == name.npos ? name.size() - offset : end - offset);
  Require(!part.empty() && part != "." && part != ".." && part.back() != '.', "invalid-package-path");
  auto stem = std::string(part.substr(0, part.find('.'))); std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(toupper(c)); });
  Require(stem != "CON" && stem != "PRN" && stem != "AUX" && stem != "NUL" && !(stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '0' && stem[3] <= '9'), "reserved-package-name");
  if (end == name.npos) break; offset = end + 1;
 }
}
void VerifyPackage(const fs::path& path, const Manifest& manifest)
{
 auto file = OpenLocked(path); LARGE_INTEGER size{};
 Require(GetFileSizeEx(file.handle, &size) && size.QuadPart >= 0 && static_cast<uint64_t>(size.QuadPart) == manifest.package_size && FileSha256(file.handle) == manifest.package_sha256, "package-integrity-failed");
}
} // namespace LauncherUpdate
