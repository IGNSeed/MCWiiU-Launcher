#include "UserDataPath.h"

#include <windows.h>
#include <shlobj.h>

#include <memory>
#include <stdexcept>

namespace mcwiiu
{
std::filesystem::path WebViewStoragePath()
{
    PWSTR rawPath = nullptr;
    const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &rawPath);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> ownedPath(rawPath, CoTaskMemFree);
    if (FAILED(result) || !ownedPath)
        throw std::runtime_error("USER_DATA_PATH_UNAVAILABLE");

    auto path = std::filesystem::path(ownedPath.get()) / L"MCWiiU Launcher" / L"WebView2";
    std::filesystem::create_directories(path);
    return path;
}
}
