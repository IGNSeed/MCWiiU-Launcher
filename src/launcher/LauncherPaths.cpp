#include "LauncherPaths.h"
#include "NativeSettings.h"

#include <ShlObj.h>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}
	bool Missing(DWORD error)
	{
		return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
	}
	void OrdinaryPath(const fs::path& path)
	{
		for (auto current = path; !current.empty(); current = current.parent_path())
		{
			const auto attributes = GetFileAttributesW(current.c_str());
			if (attributes == INVALID_FILE_ATTRIBUTES)
				Require(Missing(GetLastError()), "Cannot inspect application data permissions");
			else
				Require(!(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "Application data must not contain symbolic links or junctions");
			if (current == current.root_path()) break;
		}
	}
} // namespace

fs::path LauncherPaths::LocalRoot()
{
	PWSTR value = nullptr;
	const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &value);
	fs::path local;
	if (SUCCEEDED(result) && value) local = value;
	CoTaskMemFree(value);
	Require(!local.empty() && local.is_absolute(), "Windows LocalAppData is unavailable");
	return local / L"MCWiiU-Launcher";
}

LauncherPaths& LauncherPaths::Get()
{
	static LauncherPaths paths(LocalRoot());
	return paths;
}

LauncherPaths::LauncherPaths(fs::path directory)
	: root(std::move(directory)), data(root / L"Data"), cache(root / L"Cache"), logs(root / L"Logs"), webview(root / L"WebView2")
{
	Require(root.is_absolute() && root == root.lexically_normal(), "Invalid application data root");
}

LauncherPaths::~LauncherPaths()
{
	for (const auto handle : m_directories) CloseHandle(handle);
}

void LauncherPaths::CheckPath(const fs::path& path) const
{
	const auto relative = path.lexically_normal().lexically_relative(root);
	Require(path.is_absolute() && !relative.empty() && *relative.begin() != L"..", "Path escaped application data");
	OrdinaryPath(path);
}

bool LauncherPaths::ContainsData(const fs::path& path) const
{
	if (!path.is_absolute()) return false;
	const auto relative = path.lexically_normal().lexically_relative(data);
	return !relative.empty() && *relative.begin() != L"..";
}

void LauncherPaths::ValidateTree() const
{
	CheckPath(root);
	if (!fs::exists(root)) return;
	// iterator はリンクを追跡しない。Core が走査する前に全ての既存リンクを拒否する。
	for (const auto& entry : fs::recursive_directory_iterator(root))
	{
		CheckPath(entry.path());
		if (!entry.is_directory())
		{
			// 終了中の WebView2 がロックしたファイルも、内容を開かず属性だけ調べる。
			const auto handle = CreateFileW(entry.path().c_str(), 0,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
			if (handle == INVALID_HANDLE_VALUE && Missing(GetLastError())) continue;
			Require(handle != INVALID_HANDLE_VALUE, "Cannot inspect application data file");
			BY_HANDLE_FILE_INFORMATION info{};
			const bool safe = GetFileInformationByHandle(handle, &info) && info.nNumberOfLinks == 1 && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
			CloseHandle(handle);
			Require(safe, "Application data must not contain linked files");
		}
	}
}

void LauncherPaths::CreateDefault(const fs::path& path, std::string_view bytes) const
{
	CheckPath(path);
	const auto attributes = GetFileAttributesW(path.c_str());
	if (attributes != INVALID_FILE_ATTRIBUTES)
	{
		Require(!(attributes & FILE_ATTRIBUTE_DIRECTORY), "A settings path is a directory");
		return;
	}
	Require(Missing(GetLastError()), "Cannot inspect settings file");
	auto temporary = path;
	temporary += L".initializing";
	CheckPath(temporary);
	const auto handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	Require(handle != INVALID_HANDLE_VALUE, "Cannot create initial settings; existing temporary files were preserved");
	DWORD written = 0;
	const bool flushed = WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(handle);
	const bool closed = CloseHandle(handle) != FALSE;
	// REPLACE_EXISTING を使わず、初期化による既存設定の上書きを防ぐ。
	const bool moved = flushed && closed && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH);
	if (!moved) DeleteFileW(temporary.c_str());
	Require(moved, "Cannot persist initial settings; existing settings were preserved");
}

void LauncherPaths::Initialize()
{
	if (m_initialized) return;
	ValidateTree();
	for (const auto& directory : {root, data, cache, logs, webview, data / L"mlc01", data / L"graphicPacks", data / L"Textures", data / L"controllerProfiles"})
	{
		CheckPath(directory);
		fs::create_directories(directory);
		CheckPath(directory);
		const auto handle = CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
			nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		Require(handle != INVALID_HANDLE_VALUE, "Cannot protect application data directory");
		BY_HANDLE_FILE_INFORMATION info{};
		const bool ordinary = GetFileInformationByHandle(handle, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
		if (!ordinary) CloseHandle(handle);
		Require(ordinary, "Application data directory changed during initialization");
		m_directories.push_back(handle);
		CheckPath(directory);
		const auto probe = directory / (L".write-check-" + std::to_wstring(GetCurrentProcessId()));
		const auto file = CreateFileW(probe.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
			FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
		Require(file != INVALID_HANDLE_VALUE, "Application data directory is not writable");
		CloseHandle(file);
	}
	CreateDefault(data / L"settings.xml", "<?xml version=\"1.0\" encoding=\"UTF-8\"?><content><play_boot_sound>true</play_boot_sound><Graphic><api>1</api></Graphic><Audio/></content>\n");
	const NativeSettings defaults;
	CreateDefault(data / L"launcher-native-settings.json", "{\"config_version\":1,\"native_vsync_frequency\":" + std::to_string(defaults.vsyncFrequency) +
		",\"native_internal_width\":" + std::to_string(defaults.width) + ",\"native_internal_height\":" + std::to_string(defaults.height) + "}\n");
	m_initialized = true;
}
