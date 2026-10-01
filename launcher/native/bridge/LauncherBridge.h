#pragma once

#include "runtime/MinecraftRuntime.h"

#include <cstdint>
#include <string>

namespace mcwiiu
{
inline constexpr std::uint32_t BridgeProtocolVersion = 1;

// Only public, non-sensitive data crosses the webview boundary.
struct AppInfo
{
    std::string name;
    std::string architecture;
    std::string configuration;
    bool development;
    std::string runtimeState;
    bool runtimeIntegrated;
    std::uint32_t bridgeProtocolVersion;
};

class LauncherBridge final
{
public:
    explicit LauncherBridge(const MinecraftRuntime& runtime) noexcept;
    [[nodiscard]] AppInfo GetAppInfo() const;
    [[nodiscard]] std::string GetRuntimeState() const;

private:
    const MinecraftRuntime& runtime_;
};
}
