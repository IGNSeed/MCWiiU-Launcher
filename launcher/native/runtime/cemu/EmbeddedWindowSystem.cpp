#include "gui/interface/WindowSystem.h"

namespace
{
struct EmbeddedWindowState
{
    WindowSystem::WindowInfo info{};

    EmbeddedWindowState()
    {
        info.dpi_scale = 1.0;
        info.pad_dpi_scale = 1.0;
        info.window_main.backend = info.window_pad.backend =
            info.canvas_main.backend = info.canvas_pad.backend = WindowSystem::WindowHandleInfo::Backend::Windows;
    }
} windowState;

void Unsupported(std::string_view operation)
{
    cemuLog_log(LogType::Force, "Launcher WindowSystem: {} is unavailable without a game window", operation);
}
}

namespace WindowSystem
{
void ShowErrorDialog(std::string_view message, std::string_view title, std::optional<ErrorCategory>)
{
    cemuLog_log(LogType::Force, "Launcher core error: {} {}", title, message);
}

void Create() { Unsupported("Create"); }
WindowInfo& GetWindowInfo() { return windowState.info; }
void UpdateWindowTitles(bool, bool, double) { Unsupported("UpdateWindowTitles"); }
void GetWindowSize(int& w, int& h) { w = windowState.info.width; h = windowState.info.height; }
void GetPadWindowSize(int& w, int& h) { w = 0; h = 0; }
void GetWindowPhysSize(int& w, int& h) { w = windowState.info.phys_width; h = windowState.info.phys_height; }
void GetPadWindowPhysSize(int& w, int& h) { w = 0; h = 0; }
double GetWindowDPIScale() { return 1.0; }
double GetPadDPIScale() { return 1.0; }
bool IsPadWindowOpen() { return false; }
bool IsKeyDown(uint32) { return false; }
bool IsKeyDown(PlatformKeyCodes) { return false; }
std::string GetKeyCodeName(uint32 key) { return fmt::format("key_{}", key); }
bool InputConfigWindowHasFocus() { return false; }
void NotifyGameLoaded() { Unsupported("NotifyGameLoaded"); }
void NotifyGameExited() { Unsupported("NotifyGameExited"); }
void RefreshGameList() { Unsupported("RefreshGameList"); }
bool IsFullScreen() { return false; }
void CaptureInput(const ControllerState&, const ControllerState&) { Unsupported("CaptureInput"); }
}
