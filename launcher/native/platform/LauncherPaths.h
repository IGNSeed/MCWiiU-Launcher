#pragma once

#include <filesystem>

namespace mcwiiu
{
struct LauncherPaths
{
    std::filesystem::path root;
    std::filesystem::path config;
    std::filesystem::path runtime;
    std::filesystem::path mlc;
    std::filesystem::path cache;
    std::filesystem::path logs;
    std::filesystem::path webView;
    std::filesystem::path mods;
    std::filesystem::path textures;
    std::filesystem::path executable;
    std::filesystem::path resources;

    void CreateDirectories() const;
};

[[nodiscard]] LauncherPaths ResolveLauncherPaths();
}
