#include <saucer/webview.hpp>
#include <saucer/embedded/all.hpp>

#include <Windows.h>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	int s_startupExitCode = 0;

	// 例外経路でも WebView2 の破棄より先に通知の参照を解除する。
	struct WindowNotifications
	{
		saucer::window& m_window;
		saucer::webview& m_webview;
		bool m_closing = false;
		std::optional<size_t> m_maximizeListener;
		std::optional<size_t> m_closedListener;

		~WindowNotifications()
		{
			m_closing = true;
			if (m_maximizeListener)
				m_window.off(saucer::window::event::maximize, *m_maximizeListener);
			if (m_closedListener)
				m_window.off(saucer::window::event::closed, *m_closedListener);
			m_webview.off(saucer::webview::event::message);
			m_webview.off(saucer::webview::event::dom_ready);
		}
	};

	void ReportStartupFailure()
	{
		s_startupExitCode = 1;
		MessageBoxW(nullptr, L"LauncherShell could not start. Check the WebView2 Runtime and the application data directory.", L"MCWiiU Launcher", MB_OK | MB_ICONERROR);
	}

	std::filesystem::path UserDataPath()
	{
		std::vector<wchar_t> path(32768);
		const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
		if (!length || length == path.size())
			throw std::runtime_error("Unable to locate LauncherShell");
		return std::filesystem::path(std::wstring(path.data(), length)).parent_path() / "user-data";
	}

	coco::stray Start(saucer::application* app)
	{
		try
		{
			auto window = saucer::window::create(app).value();
			window->set_title("MCWiiU Launcher");
			window->set_decorations(saucer::window::decoration::partial);
			window->set_size({1120, 760});
			window->set_min_size({820, 600});
			window->set_background({13, 13, 15, 255});

			const saucer::webview::options options{
				.window = window,
				.attributes = false,
				.persistent_cookies = true,
				.storage_path = UserDataPath(),
				.browser_flags = {"--disable-background-networking", "--no-first-run"},
			};
			auto webview = saucer::webview::create(options).value();
			webview.set_background({13, 13, 15, 255});
			webview.set_dev_tools(false);
			webview.set_context_menu(false);
			// 遷移取消だけでは先行する通信を防げないため、Web リソース要求も拒否する。
			const auto denyNetworkRequest = [](saucer::scheme::request, saucer::scheme::executor executor) {
				executor.resolve(saucer::scheme::response{.data = saucer::stash::empty(), .mime = "text/plain", .status = 403});
			};
			webview.handle_scheme("http", denyNetworkRequest);
			webview.handle_scheme("https", denyNetworkRequest);
			webview.on<saucer::webview::event::navigate>([](const saucer::navigation& navigation) {
				const auto url = navigation.url();
				return !navigation.new_window() && url.scheme() == "saucer" && url.host() == "embedded" && url.path() == "/index.html"
						   ? saucer::policy::allow
						   : saucer::policy::block;
			});
			WindowNotifications notifications{*window, webview};
			const auto updateWindowState = [&] {
				if (!notifications.m_closing)
					webview.execute(window->maximized()
										? "window.dispatchEvent(new CustomEvent('launcher-window-state', {detail: true}));"
										: "window.dispatchEvent(new CustomEvent('launcher-window-state', {detail: false}));");
			};
			notifications.m_maximizeListener = window->on<saucer::window::event::maximize>([updateWindowState](bool) { updateWindowState(); });
			notifications.m_closedListener = window->on<saucer::window::event::closed>([&] { notifications.m_closing = true; });
			webview.on<saucer::webview::event::dom_ready>(updateWindowState);
			webview.on<saucer::webview::event::message>([&](std::string_view message) {
				const auto url = webview.url();
				if (notifications.m_closing || url.scheme() != "saucer" || url.host() != "embedded" || url.path() != "/index.html")
					return saucer::status::unhandled;

				// 埋め込みページからの完全一致するウィンドウ操作だけを受け付ける。
				if (message == "window:minimize")
					window->set_minimized(true);
				else if (message == "window:toggle-maximize")
					window->set_maximized(!window->maximized());
				else if (message == "window:close")
					window->close();
				else if (message == "window:start-drag")
				{
					if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)
						window->start_drag();
				}
				else
					return saucer::status::unhandled;
				return saucer::status::handled;
			});
			// フロントエンドは埋め込み済みの3ファイルだけを使う。
			webview.embed(saucer::embedded::all());
			webview.serve("/index.html");
			window->set_maximized(true);
			window->show();
			co_await app->finish();
		} catch (const std::exception&)
		{
			ReportStartupFailure();
			app->quit();
		}
	}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	try
	{
		auto application = saucer::application::create({.id = "MCWiiU-Launcher"}).value();
		const auto result = application.run(Start);
		return s_startupExitCode ? s_startupExitCode : result;
	} catch (const std::exception&)
	{
		ReportStartupFailure();
		return 1;
	}
}
