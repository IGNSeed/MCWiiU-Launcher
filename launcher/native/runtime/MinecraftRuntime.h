#pragma once

#include <string_view>
#include "cemu/CemuRuntimeBackend.h"

namespace mcwiiu
{
enum class RuntimeState
{
    Uninitialized,
    Initializing,
    Ready,
    Launching,
    Running,
    Stopping,
    Error
};

// Lifecycle operations belong on the runtime owner thread, never the render loop.
class MinecraftRuntime final
{
public:
    MinecraftRuntime() = default;
    ~MinecraftRuntime();
    MinecraftRuntime(const MinecraftRuntime&) = delete;
    MinecraftRuntime& operator=(const MinecraftRuntime&) = delete;

    [[nodiscard]] bool Initialize() noexcept;
    void Shutdown() noexcept;
    [[nodiscard]] RuntimeState GetState() const noexcept;
    [[nodiscard]] bool IsIntegrated() const noexcept;
    [[nodiscard]] RuntimeError GetError() const noexcept;

private:
    RuntimeState state_ = RuntimeState::Uninitialized;
    CemuRuntimeBackend backend_;
    bool attempted_ = false;
    RuntimeError error_ = RuntimeError::None;
};

[[nodiscard]] std::string_view RuntimeStateName(RuntimeState state) noexcept;
[[nodiscard]] std::string_view RuntimeErrorName(RuntimeError error) noexcept;
}
