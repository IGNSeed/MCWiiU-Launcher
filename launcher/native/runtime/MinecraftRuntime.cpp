#include "MinecraftRuntime.h"

namespace mcwiiu
{
MinecraftRuntime::~MinecraftRuntime()
{
    Shutdown();
}

bool MinecraftRuntime::Initialize() noexcept
{
    if (attempted_)
        return false;
    attempted_ = true;
    state_ = RuntimeState::Initializing;
    try
    {
        if (backend_.Initialize(ResolveLauncherPaths()))
        {
            state_ = RuntimeState::Ready;
            return true;
        }
        error_ = backend_.GetError();
    }
    catch (...)
    {
        // Keep the launcher UI available when path resolution fails.
        error_ = RuntimeError::PathSetupFailed;
    }
    state_ = RuntimeState::Error;
    return false;
}

void MinecraftRuntime::Shutdown() noexcept
{
    backend_.Shutdown();
    if (state_ != RuntimeState::Error)
        state_ = RuntimeState::Uninitialized;
}

RuntimeState MinecraftRuntime::GetState() const noexcept
{
    return state_;
}

bool MinecraftRuntime::IsIntegrated() const noexcept
{
    return true;
}

RuntimeError MinecraftRuntime::GetError() const noexcept
{
    return error_;
}

std::string_view RuntimeErrorName(RuntimeError error) noexcept
{
    switch (error)
    {
    case RuntimeError::None: return "NONE";
    case RuntimeError::PathSetupFailed: return "PATH_SETUP_FAILED";
    case RuntimeError::MLCSetupFailed: return "MLC_SETUP_FAILED";
    case RuntimeError::CoreInitializationFailed: return "CORE_INITIALIZATION_FAILED";
    }
    return "CORE_INITIALIZATION_FAILED";
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
