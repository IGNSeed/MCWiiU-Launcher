#include <saucer/webview.hpp>
#include <saucer/embedded/all.hpp>

#include <Windows.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	int s_startupExitCode = 0;

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
			// フロントエンドは埋め込み済みの3ファイルだけを使い、ネイティブ API を公開しない。
			webview.embed(saucer::embedded::all());
			webview.serve("/index.html");
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
