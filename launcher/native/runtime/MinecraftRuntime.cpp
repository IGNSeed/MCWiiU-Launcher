#include "MinecraftRuntime.h"

namespace mcwiiu
{
MinecraftRuntime::~MinecraftRuntime()
{
    Shutdown();
}

bool MinecraftRuntime::Initialize() noexcept
{
    // Do not claim Ready until the in-process Cemu backend has initialized.
    return false;
}

void MinecraftRuntime::Shutdown() noexcept
{
    state_ = RuntimeState::Uninitialized;
}

RuntimeState MinecraftRuntime::GetState() const noexcept
{
    return state_;
}

bool MinecraftRuntime::IsIntegrated() const noexcept
{
    return false;
}

std::string_view RuntimeStateName(RuntimeState state) noexcept
{
    switch (state)
    {
    case RuntimeState::Uninitialized: return "Uninitialized";
    case RuntimeState::Initializing: return "Initializing";
    case RuntimeState::Ready: return "Ready";
    case RuntimeState::Launching: return "Launching";
    case RuntimeState::Running: return "Running";
    case RuntimeState::Stopping: return "Stopping";
    case RuntimeState::Error: return "Error";
    }
    return "Error";
}
}
