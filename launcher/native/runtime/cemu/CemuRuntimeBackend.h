#pragma once

#include "platform/LauncherPaths.h"
#include <memory>

namespace mcwiiu
{
enum class RuntimeError
{
    None,
    PathSetupFailed,
    MLCSetupFailed,
    CoreInitializationFailed
};

class CemuRuntimeBackend final
{
public:
    CemuRuntimeBackend();
    ~CemuRuntimeBackend();
    CemuRuntimeBackend(const CemuRuntimeBackend&) = delete;
    CemuRuntimeBackend& operator=(const CemuRuntimeBackend&) = delete;

    [[nodiscard]] bool Initialize(const LauncherPaths& paths) noexcept;
    void Shutdown() noexcept;
    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] RuntimeError GetError() const noexcept;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
    RuntimeError error_ = RuntimeError::None;
    bool attempted_ = false;
    bool initialized_ = false;
};
}
