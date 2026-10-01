#include "CemuRuntimeBackend.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cemu/Runtime/MLC.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "config/ActiveSettings.h"
#include "util/crypto/aes128.h"

namespace mcwiiu
{
class CemuRuntimeBackend::Implementation final : public CafeSystem::SystemImplementation
{
public:
    void CafeRecreateCanvas() override
    {
        cemuLog_log(LogType::Force, "Launcher runtime: canvas recreation is unavailable in this phase");
    }

    void CafePPCProcessExit() override
    {
        cemuLog_log(LogType::Force, "Launcher runtime: unexpected emulated process exit without a game session");
    }
};

CemuRuntimeBackend::CemuRuntimeBackend() = default;

CemuRuntimeBackend::~CemuRuntimeBackend()
{
    Shutdown();
}

bool CemuRuntimeBackend::Initialize(const LauncherPaths& paths) noexcept
{
    // Cemu globals have process lifetime; a new process is required after shutdown.
    if (attempted_)
        return false;
    attempted_ = true;
    static std::atomic_bool processAttempted = false;
    if (processAttempted.exchange(true))
    {
        error_ = RuntimeError::CoreInitializationFailed;
        return false;
    }
    try
    {
        error_ = RuntimeError::PathSetupFailed;
        paths.CreateDirectories();
        std::set<fs::path> failedWriteAccess;
        ActiveSettings::SetPaths(false, paths.executable, paths.runtime, paths.config,
            paths.cache, paths.resources, failedWriteAccess);
        if (!failedWriteAccess.empty())
            return false;

        cemuLog_createLogFile(false);
        cemuLog_log(LogType::Force, "Launcher runtime initialization started");
        cemuLog_log(LogType::Force, "Launcher paths configured");
        error_ = RuntimeError::MLCSetupFailed;
        if (!Cemu::Runtime::CreateDefaultMLCFiles(paths.mlc))
        {
            cemuLog_log(LogType::Force, "Launcher runtime initialization failed: MLC_SETUP_FAILED");
            return false;
        }
        cemuLog_log(LogType::Force, "MLC ready");

        error_ = RuntimeError::CoreInitializationFailed;
        implementation_ = std::make_unique<Implementation>();
        AES128_init();
        PPCTimer_init();
        ExceptionHandler_Init();
        CafeSystem::SetImplementation(implementation_.get());
        cemuLog_log(LogType::Force, "Cemu persistent core initialization started");
        CafeSystem::Initialize();
        initialized_ = true;
        error_ = RuntimeError::None;
        cemuLog_log(LogType::Force, "Cemu persistent core ready");
        return true;
    }
    catch (...)
    {
        // Paths and exception messages may contain private values; never bridge them.
        OutputDebugStringA("Launcher runtime initialization failed\n");
        error_ = error_ == RuntimeError::None ? RuntimeError::CoreInitializationFailed : error_;
        Shutdown();
        CafeSystem::SetImplementation(nullptr);
        return false;
    }
}

void CemuRuntimeBackend::Shutdown() noexcept
{
    if (!initialized_)
        return;
    cemuLog_log(LogType::Force, "Cemu persistent core shutdown");
    CafeSystem::Shutdown();
    CafeSystem::SetImplementation(nullptr);
    initialized_ = false;
    implementation_.reset();
    cemuLog_log(LogType::Force, "Cemu persistent core shutdown complete");
    cemuLog_waitForFlush();
}

bool CemuRuntimeBackend::IsInitialized() const noexcept { return initialized_; }
RuntimeError CemuRuntimeBackend::GetError() const noexcept { return error_; }
}
