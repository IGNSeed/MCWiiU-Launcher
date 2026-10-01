#include "UserDataPath.h"
#include "platform/LauncherPaths.h"

#include <windows.h>
#include <shlobj.h>

#include <memory>
#include <stdexcept>

namespace mcwiiu
{
LauncherPaths ResolveLauncherPaths()
{
    PWSTR rawPath = nullptr;
    const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &rawPath);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> ownedPath(rawPath, CoTaskMemFree);
    if (FAILED(result) || !ownedPath)
        throw std::runtime_error("USER_DATA_PATH_UNAVAILABLE");

    const auto root = std::filesystem::path(ownedPath.get()) / L"MCWiiU Launcher";
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size())
        throw std::runtime_error("EXECUTABLE_PATH_UNAVAILABLE");
    executable.resize(length);
    const std::filesystem::path executablePath(executable);
    return {
        .root = root,
        .config = root / L"config",
        .runtime = root / L"runtime",
        .mlc = root / L"runtime" / L"mlc01",
        .cache = root / L"cache",
        .logs = root / L"logs",
        .webView = root / L"WebView2",
        .mods = root / L"mods",
        .textures = root / L"textures",
        .executable = executablePath,
        .resources = executablePath.parent_path()
    };
}

void LauncherPaths::CreateDirectories() const
{
    for (const auto& directory : {root, config, runtime, cache, logs, webView, mods, textures})
        std::filesystem::create_directories(directory);
}

std::filesystem::path WebViewStoragePath()
{
    const auto paths = ResolveLauncherPaths();
    std::filesystem::create_directories(paths.webView);
    return paths.webView;
}
}
