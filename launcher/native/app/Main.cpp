#include "bridge/LauncherBridge.h"
#include "platform/windows/UserDataPath.h"

#include <saucer/smartview.hpp>
#include <saucer/embedded/all.hpp>

#include <windows.h>

#include <exception>

namespace
{
void ReportStartupError(const wchar_t* message)
{
    MessageBoxW(nullptr, message, L"MCWiiU Launcher", MB_OK | MB_ICONERROR);
}

coco::stray Start(saucer::application* app, int& exitCode)
{
    try
    {
        mcwiiu::MinecraftRuntime runtime;
        mcwiiu::LauncherBridge bridge(runtime);
        auto window = saucer::window::create(app);
        if (!window)
        {
            ReportStartupError(L"Unable to create the launcher window.");
            exitCode = 1;
            app->quit();
            co_return;
        }

        auto webview = saucer::smartview::create({
            .window = *window,
            .persistent_cookies = false,
            .storage_path = mcwiiu::WebViewStoragePath()
        });
        if (!webview)
        {
            ReportStartupError(L"Unable to create WebView2. Install the Microsoft Edge WebView2 Runtime and try again.");
            exitCode = 1;
            app->quit();
            co_return;
        }

        (*window)->set_title("MCWiiU Launcher");
        (*window)->set_size({1100, 720});
        (*window)->set_min_size({800, 560});
        webview->set_dev_tools(false);
        webview->set_context_menu(false);
        webview->on<saucer::webview::event::navigate>([](const saucer::navigation& navigation)
        {
            const auto url = navigation.url();
            return !navigation.new_window() && url.scheme() == "saucer" && url.host() == "embedded"
                ? saucer::policy::allow : saucer::policy::block;
        });
        webview->expose("getAppInfo", [&bridge] { return bridge.GetAppInfo(); });
        webview->expose("getRuntimeState", [&bridge] { return bridge.GetRuntimeState(); });
#if MCWIIU_DEVELOPMENT
        // Saucer v8.0.5's WebView2 backend enables tools and calls OpenDevToolsWindow.
        webview->expose("openDevTools", [&webview] { webview->set_dev_tools(true); });
#endif
        webview->embed(saucer::embedded::all());
        webview->serve("/index.html");
        (*window)->show();
        co_await app->finish();
    }
    catch (const std::exception&)
    {
        ReportStartupError(L"Launcher initialization failed.");
        exitCode = 1;
        app->quit();
    }
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try
    {
        auto app = saucer::application::create({.id = "com.ignseed.mcwiiu-launcher"});
        if (!app)
        {
            ReportStartupError(L"Unable to initialize the launcher application.");
            return 1;
        }
        int exitCode = 0;
        const auto loopCode = app->run([&exitCode](saucer::application* application)
        {
            return Start(application, exitCode);
        });
        return exitCode != 0 ? exitCode : loopCode;
    }
    catch (const std::exception&)
    {
        ReportStartupError(L"Launcher startup failed.");
        return 1;
    }
}
