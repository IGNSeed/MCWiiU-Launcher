#pragma once

#include <string_view>

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

// Cemu integration is deliberately unavailable in Architecture v1.
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

private:
    RuntimeState state_ = RuntimeState::Uninitialized;
};

[[nodiscard]] std::string_view RuntimeStateName(RuntimeState state) noexcept;
}
