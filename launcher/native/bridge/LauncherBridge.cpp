#include "LauncherBridge.h"

namespace mcwiiu
{
LauncherBridge::LauncherBridge(const MinecraftRuntime& runtime) noexcept : runtime_(runtime)
{
}

AppInfo LauncherBridge::GetAppInfo() const
{
    return {"MCWiiU Launcher", "Runtime Foundation", MCWIIU_BUILD_CONFIGURATION,
        MCWIIU_DEVELOPMENT != 0, GetRuntimeState(), runtime_.IsIntegrated(), BridgeProtocolVersion};
}

std::string LauncherBridge::GetRuntimeState() const
{
    return std::string(RuntimeStateName(runtime_.GetState()));
}
}
