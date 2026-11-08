#include <saucer/webview.hpp>
#include <saucer/embedded/all.hpp>
#include <saucer/modules/stable/webview2.hpp>

#include "NativeGameHost.h"
#include "LauncherPreferences.h"
#ifdef CEMU_NATIVE_HOST
#include "CemuHost.h"
#endif

#include <Windows.h>
#include <shobjidl.h>
#include <wrl.h>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	int s_startupExitCode = 0;
	bool s_nativeHostDiagnostics = false;
	bool s_failHostInitialization = false;
	bool s_guiOnly = false;
	bool s_launchImmediately = false;
	bool s_sessionDiagnostics = false;

	std::optional<std::pair<uint64_t, std::string_view>> ParseSaveRequest(std::string_view message)
	{
		const auto separator = message.find(':');
		if (separator == std::string_view::npos) return std::nullopt;
		uint64_t id = 0;
		const auto [end, error] = std::from_chars(message.data(), message.data() + separator, id);
		if (error != std::errc{} || end != message.data() + separator || !id || id > 9007199254740991ULL)
			return std::nullopt;
		return std::pair{id, message.substr(separator + 1)};
	}

	// 例外経路でも WebView2 の破棄より先に通知の参照を解除する。
	struct WindowNotifications
	{
		saucer::window& m_window;
		saucer::webview& m_webview;
		bool m_closing = false;
		bool m_frontendReady = false, m_closeRequested = false, m_closeApproved = false;
		std::optional<size_t> m_maximizeListener;
		std::optional<size_t> m_closedListener;
		std::optional<size_t> m_closeListener;
		ICoreWebView2Controller* m_controller = nullptr;
		std::optional<EventRegistrationToken> m_acceleratorListener;
		ICoreWebView2* m_nativeWebView = nullptr;
		std::optional<EventRegistrationToken> m_dropListener;

		~WindowNotifications()
		{
			m_closing = true;
			if (m_dropListener) m_nativeWebView->remove_WebMessageReceived(*m_dropListener);
			if (m_acceleratorListener)
				m_controller->remove_AcceleratorKeyPressed(*m_acceleratorListener);
			if (m_maximizeListener)
				m_window.off(saucer::window::event::maximize, *m_maximizeListener);
			if (m_closedListener)
				m_window.off(saucer::window::event::closed, *m_closedListener);
			if (m_closeListener)
				m_window.off(saucer::window::event::close, *m_closeListener);
			m_webview.off(saucer::webview::event::message);
			m_webview.off(saucer::webview::event::dom_ready);
		}
	};

#ifdef CEMU_NATIVE_HOST
	std::optional<std::wstring> SelectDumplingZip(HWND owner, bool network = false)
	{
		Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
		if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return std::nullopt;
		const COMDLG_FILTERSPEC types[]{{network ? L"Cemu network configuration" : L"Dumpling ZIP", network ? L"*.xml" : L"*.zip"}};
		if (FAILED(dialog->SetOptions(FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT)) ||
			FAILED(dialog->SetFileTypes(1, types)) || FAILED(dialog->SetTitle(network ? L"Select network_services.xml" : L"Select Dumpling ZIP")) || FAILED(dialog->Show(owner))) return std::nullopt;
		Microsoft::WRL::ComPtr<IShellItem> item;
		PWSTR path = nullptr;
		if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
		std::wstring result(path); CoTaskMemFree(path);
		return result;
	}

	std::optional<uint64_t> ParseStopId(std::string_view value)
	{
		uint64_t id = 0;
		const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), id);
		if (value.empty() || error != std::errc{} || end != value.data() + value.size() || !id) return std::nullopt;
		return id;
	}
	std::optional<std::pair<uint64_t, uint64_t>> ParseStopConfirmation(std::string_view value)
	{
		const auto separator = value.find(':');
		if (separator == std::string_view::npos) return std::nullopt;
		const auto session = ParseStopId(value.substr(0, separator));
		const auto confirmation = ParseStopId(value.substr(separator + 1));
		if (!session || !confirmation) return std::nullopt;
		return std::pair{*session, *confirmation};
	}

	// UI STA 内だけで表示状態を確定する。配置の保存・復元は saucer に任せる。
	struct GameDisplayState
	{
		enum class Mode { LauncherWindowed, GameFullscreen };
		saucer::window& m_window;
		saucer::webview& m_webview;
		NativeGameHost& m_host;
		CemuHost& m_core;
		Mode m_mode = Mode::LauncherWindowed;
		bool m_transitioning = false, m_stopping = false, m_settingsSelected = false;
		bool m_webViewWasVisible = true, m_restoreGameFocus = false;
		bool m_launcherVisible = false, m_stopConfirmation = false, m_boundsSyncPending = false;
		uint64_t m_confirmationSession = 0, m_confirmationId = 0;
		std::optional<WINDOWPLACEMENT> m_maximizedPlacement;

		bool IsFullscreen() const { return m_mode == Mode::GameFullscreen; }

		void Publish()
		{
			BOOL visible = FALSE;
			m_webview.native().controller->get_IsVisible(&visible);
			m_webview.execute(std::string("window.dispatchEvent(new CustomEvent('launcher-display-state', {detail:{mode:'") +
				(IsFullscreen() ? "GameFullscreen" : "LauncherWindowed") + "',webViewVisible:" + (visible ? "true" : "false") + "}}));");
		}

		void Fail()
		{
			OutputDebugStringW(L"MCWiiU Launcher: display transition failed; stopping Core before destroying UI\n");
			m_stopping = true;
			m_core.ReportDisplayFailure();
		}

		bool SetFullscreen(bool enabled, bool launcherFocus = false)
		{
			if (m_transitioning || m_stopping || m_stopConfirmation || !m_core.IsRunning()) return false;
			if (enabled == IsFullscreen()) return true;
			m_transitioning = true;
			auto* controller = m_webview.native().controller;
			// held key と VPAD/KPAD gate を閉じてから表示・フォーカスを変更する。
			m_host.CancelTouch();
			m_core.InputMessage(WM_CANCELMODE, 0, 0);
			bool success = true;
			if (enabled)
			{
				MONITORINFO monitor{sizeof(MONITORINFO)};
				success = GetMonitorInfoW(MonitorFromWindow(m_window.native().hwnd, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE;
				BOOL visible = FALSE;
				success = SUCCEEDED(controller->get_IsVisible(&visible)) && success;
				m_webViewWasVisible = visible != FALSE;
				m_restoreGameFocus = GetFocus() == m_host.GetHandle();
				// saucer は style 更新後に最大化状態と通常配置を失うため、この場合だけ補完する。
				m_maximizedPlacement.reset();
				if (m_window.maximized())
				{
					WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
					success = GetWindowPlacement(m_window.native().hwnd, &placement) && success;
					if (success) m_maximizedPlacement = placement;
				}
				if (success) success = SUCCEEDED(controller->put_IsVisible(FALSE));
				if (success)
				{
					m_mode = Mode::GameFullscreen;
					success = m_host.SetFullscreenLayout(true);
					m_window.set_fullscreen(true);
					RECT root{};
					success = GetWindowRect(m_window.native().hwnd, &root) && success;
					// saucer 8.0.5 はモニター原点の x/y を逆転する。異なる DPI への
					// 移動で再計算された寸法も含め、元の物理モニター矩形だけを補完する。
					for (unsigned attempt = 0; success && !EqualRect(&root, &monitor.rcMonitor) && attempt < 2; ++attempt)
					{
						success = SetWindowPos(m_window.native().hwnd, nullptr, monitor.rcMonitor.left, monitor.rcMonitor.top,
							monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
							SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED) != FALSE;
						success = GetWindowRect(m_window.native().hwnd, &root) && success;
					}
					m_host.SetVisible(true);
					success = EqualRect(&root, &monitor.rcMonitor) &&
						success && m_host.Focus();
				}
			}
			else
			{
				success = m_host.SetFullscreenLayout(false);
				m_window.set_fullscreen(false);
				if (m_maximizedPlacement) success = SetWindowPlacement(m_window.native().hwnd, &*m_maximizedPlacement) && success;
				m_mode = Mode::LauncherWindowed;
				m_host.SetVisible(!m_settingsSelected && !m_launcherVisible);
				RECT client{};
				success = GetClientRect(m_window.native().hwnd, &client) && success;
				if (success) success = SUCCEEDED(controller->put_Bounds(client));
				if (success) success = SUCCEEDED(controller->put_IsVisible(m_webViewWasVisible));
				if (success && !launcherFocus && m_restoreGameFocus && !m_settingsSelected && !m_launcherVisible)
					success = m_host.Focus();
				else if (success && m_webViewWasVisible)
					success = SUCCEEDED(controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC));
			}
			if (!success)
			{
				if (enabled && IsFullscreen())
				{
					m_host.SetFullscreenLayout(false);
					m_window.set_fullscreen(false);
					if (m_maximizedPlacement) SetWindowPlacement(m_window.native().hwnd, &*m_maximizedPlacement);
					m_mode = Mode::LauncherWindowed;
					m_host.SetVisible(!m_settingsSelected && !m_launcherVisible);
					RECT client{};
					if (GetClientRect(m_window.native().hwnd, &client)) controller->put_Bounds(client);
					controller->put_IsVisible(m_webViewWasVisible);
				}
				// 失敗後の再操作は止め、既存の非同期 Core/GPU 停止を優先する。
				Fail();
				m_transitioning = false;
				return false;
			}
			m_transitioning = false;
			if (m_host.stateChanged) m_host.stateChanged();
			Publish();
			return true;
		}

		void SyncWebViewBounds()
		{
			if (m_transitioning || m_stopping || IsFullscreen() || IsIconic(m_window.native().hwnd) || !IsWindowVisible(m_window.native().hwnd)) return;
			m_webview.execute(std::string("document.documentElement.dataset.gameVisible='") + ((!m_launcherVisible && !m_settingsSelected && m_core.IsRunning()) ? "true" : "false") + "';");
			RECT client{}, bounds{};
			BOOL visible = FALSE;
			auto* controller = m_webview.native().controller;
			if (!GetClientRect(m_window.native().hwnd, &client) || FAILED(controller->get_Bounds(&bounds)))
			{
				Fail();
				return;
			}
			// 最小化中の小さな Bounds が残った場合だけ、実 client の物理寸法へ戻す。
			if (client.right > 0 && client.bottom > 0 && !EqualRect(&client, &bounds) && FAILED(controller->put_Bounds(client)))
			{
				Fail();
				return;
			}
			// 最大化への復帰では saucer の minimize(false) 通知がない場合がある。
			if (FAILED(controller->get_IsVisible(&visible)) || (!visible && FAILED(controller->put_IsVisible(TRUE))))
				Fail();
		}

		void ShowLauncherHome()
		{
			if (m_stopping) return;
			if (IsFullscreen() && !SetFullscreen(false, true)) return;
			m_launcherVisible = true;
			m_settingsSelected = false;
			m_core.InputMessage(WM_CANCELMODE, 0, 0);
			m_host.SetVisible(false);
			m_webview.execute("window.dispatchEvent(new Event('launcher-show-home'));");
			if (FAILED(m_webview.native().controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC))) Fail();
		}
		void ReturnToGame()
		{
			if (m_stopping || m_transitioning || m_stopConfirmation || !m_core.IsRunning() || m_core.IsTitleStopping()) return;
			m_core.CancelInputCapture();
			m_settingsSelected = m_launcherVisible = false;
			m_webview.execute("window.dispatchEvent(new Event('launcher-return-game'));");
			m_host.SetVisible(true);
			m_host.Focus();
			SyncWebViewBounds();
		}
		void SetStopConfirmation(bool active)
		{
			if (active == m_stopConfirmation) return;
			m_stopConfirmation = active;
			if (active)
			{
				m_confirmationSession = m_core.SessionId();
				m_confirmationId = m_core.StopConfirmationId();
				m_core.CancelInputCapture();
				m_host.CancelTouch();
				m_core.InputMessage(WM_CANCELMODE, 0, 0);
				m_settingsSelected = m_launcherVisible = false;
				m_webview.execute("window.dispatchEvent(new Event('launcher-return-game'));");
				m_host.SetVisible(true);
			}
			m_host.SetExitConfirmation(active);
			if (!active && m_core.IsRunning() && !m_core.IsTitleStopping()) ReturnToGame();
			if (!active) m_confirmationSession = m_confirmationId = 0;
		}

		bool ReturnHome()
		{
			if (m_stopping) return true;
			m_host.CancelTouch();
			m_host.SetExitConfirmation(false);
			m_core.InputMessage(WM_CANCELMODE, 0, 0);
			m_transitioning = true;
			bool success = true;
			if (IsFullscreen())
			{
				success = m_host.SetFullscreenLayout(false);
				m_window.set_fullscreen(false);
				if (m_maximizedPlacement) success = SetWindowPlacement(m_window.native().hwnd, &*m_maximizedPlacement) && success;
				m_mode = Mode::LauncherWindowed;
			}
			m_settingsSelected = false;
			m_launcherVisible = true;
			m_host.SetVisible(false);
			m_host.SetGameLayout(false);
			auto* controller = m_webview.native().controller;
			RECT client{};
			success = GetClientRect(m_window.native().hwnd, &client) && success;
			if (success) success = SUCCEEDED(controller->put_Bounds(client));
			if (success) success = SUCCEEDED(controller->put_IsVisible(TRUE));
			if (success) success = SUCCEEDED(controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC));
			SetWindowTextW(m_window.native().hwnd, L"MCWiiU Launcher");
			m_webview.execute("window.dispatchEvent(new Event('launcher-session-ended'));");
			m_transitioning = false;
			if (m_host.stateChanged) m_host.stateChanged();
			Publish();
			return success;
		}

		void Shortcut(UINT key, uint64_t expectedSession)
		{
			if (m_stopping || m_transitioning || !m_core.IsRunning() || m_core.SessionId() != expectedSession) return;
			if (m_stopConfirmation)
			{
				if (key == VK_ESCAPE) m_core.CancelStopGameConfirmation(m_confirmationSession, m_confirmationId);
				return;
			}
			if (key == VK_ESCAPE && IsFullscreen()) { SetFullscreen(false); return; }
			if (key == VK_F11)
				SetFullscreen(!IsFullscreen());
			else if (key == VK_F10 || (key == VK_ESCAPE && !m_settingsSelected && !m_launcherVisible))
			{
				m_core.CancelInputCapture();
				m_core.BeginStopGameConfirmation(expectedSession);
			}
		}
	};
#endif

	void ReportStartupFailure()
	{
		s_startupExitCode = 1;
		if (s_nativeHostDiagnostics)
			return;
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
			NativeGameHost gameHost(window->native().hwnd, webview.native().controller, s_nativeHostDiagnostics, s_failHostInitialization);
			const auto preferencesPath = UserDataPath() / "launcher-preferences.json";
			std::string preferencesNotice;
			auto preferences = LauncherPreferences::Load(preferencesPath, preferencesNotice);
			const auto publishPreferences = [&] {
				webview.execute("window.dispatchEvent(new CustomEvent('launcher-preferences', {detail:{values:" + preferences.Json() +
					",notice:'" + preferencesNotice + "',version:'" LAUNCHER_VERSION "'}}));");
			};
#ifdef CEMU_NATIVE_HOST
			std::unique_ptr<CemuHost> core;
			std::unique_ptr<GameDisplayState> display;
#endif
			WindowNotifications notifications{*window, webview};
			const auto updateWindowState = [&] {
				if (!notifications.m_closing)
					webview.execute(window->maximized()
										? "window.dispatchEvent(new CustomEvent('launcher-window-state', {detail: true}));"
										: "window.dispatchEvent(new CustomEvent('launcher-window-state', {detail: false}));");
			};
			notifications.m_maximizeListener = window->on<saucer::window::event::maximize>([updateWindowState](bool) { updateWindowState(); });
			notifications.m_closedListener = window->on<saucer::window::event::closed>([&] { notifications.m_closing = true; });
#ifdef CEMU_NATIVE_HOST
			const auto publishCore = [&](const std::string& json) {
				if (!notifications.m_closing)
					webview.execute("window.dispatchEvent(new CustomEvent('launcher-core-state', {detail:" + json + "}));");
			};
			if (!s_guiOnly && !s_nativeHostDiagnostics)
			{
				core = std::make_unique<CemuHost>(window->native().hwnd, gameHost.GetHandle(), CemuHost::Callbacks{
					.post = [app](std::function<void()> callback) { app->post(std::move(callback)); },
					.prepareCanvas = [&](bool openGL) {
						if (!gameHost.SetOpenGLComposition(openGL)) return false;
						if (display) { display->m_launcherVisible = false; display->m_settingsSelected = false; }
						gameHost.SetGameLayout(true);
						gameHost.SetVisible(true);
						gameHost.Focus();
						return gameHost.IsRenderable();
					},
					.openGLCanvas = [&] { return gameHost.GetOpenGLHandle(); },
					.status = [&](std::string json) {
						if (display) display->SetStopConfirmation(core->HasStopGameConfirmation());
						publishCore(json);
						if (gameHost.stateChanged) gameHost.stateChanged();
					},
					.titleStopped = [&] { return gameHost.SetOpenGLComposition(false) && display->ReturnHome(); },
					.stopped = [&] {
						s_startupExitCode = core->HasFailed() ? 1 : 0;
						window->close();
						}
					});
				gameHost.stateChanged = [&] {
					gameHost.SetControlsVisible(display && !display->m_stopping && !display->m_transitioning &&
						!display->m_stopConfirmation && !display->m_settingsSelected && !display->m_launcherVisible && core->IsRunning());
					if (display && !display->m_boundsSyncPending)
					{
						display->m_boundsSyncPending = true;
						app->post([&] {
							display->m_boundsSyncPending = false;
							if (!notifications.m_closing) display->SyncWebViewBounds();
						});
					}
					if (display && !display->m_transitioning && core->IsRunning() && gameHost.HasLayoutFailed()) display->Fail();
					core->UpdateWindowState(gameHost.GetClientSize(), GetDpiForWindow(gameHost.GetHandle()),
						gameHost.IsRenderable(), GetFocus() == gameHost.GetHandle() && GetForegroundWindow() == window->native().hwnd &&
							!(display && (display->m_transitioning || display->m_stopConfirmation)),
						display && display->IsFullscreen());
					if (display && !display->m_transitioning && !notifications.m_closing) display->Publish();
				};
				gameHost.input = [&](UINT message, WPARAM key, LPARAM value) { core->InputMessage(message, key, value); };
				display = std::make_unique<GameDisplayState>(*window, webview, gameHost, *core);
				gameHost.exitResult = [&](bool confirm) {
					const auto session = display->m_confirmationSession, confirmation = display->m_confirmationId;
					app->post([&, confirm, session, confirmation] {
						if (confirm) core->ConfirmStopGame(session, confirmation);
						else core->CancelStopGameConfirmation(session, confirmation);
					});
				};
				gameHost.shortcut = [&](UINT key) { const auto session = core->SessionId(); app->post([&, key, session] { display->Shortcut(key, session); }); };
			}
#endif
			notifications.m_closeListener = window->on<saucer::window::event::close>([&] {
				// Alt+F4 も、WebView 内の未送信の保存要求が完了してから閉じる。
				if (notifications.m_frontendReady && !notifications.m_closeApproved)
				{
					if (!notifications.m_closeRequested)
						webview.execute("window.dispatchEvent(new Event('launcher-close-request'));");
					notifications.m_closeRequested = true;
					return saucer::policy::block;
				}
#ifdef CEMU_NATIVE_HOST
				if (display)
				{
					display->m_stopping = true;
					gameHost.SetControlsVisible(false);
					if (!core->IsStopped())
					{
						core->CancelInputCapture();
						core->RequestStop();
						publishCore(core->StatusJson());
						return saucer::policy::block;
					}
				}
#endif
				return saucer::policy::allow;
			});
			// WebView2 側だけで捕捉し、JS と二重配送しない。COM event 内では遷移しない。
			notifications.m_controller = webview.native().controller;
			EventRegistrationToken acceleratorToken{};
			const auto accelerator = Microsoft::WRL::Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
				[&](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
					UINT key = 0;
					COREWEBVIEW2_KEY_EVENT_KIND kind{};
					COREWEBVIEW2_PHYSICAL_KEY_STATUS physical{};
					if (FAILED(args->get_VirtualKey(&key)) || (key != VK_F11 && key != VK_F10 && key != VK_ESCAPE)) return S_OK;
#ifdef CEMU_NATIVE_HOST
					if (key == VK_ESCAPE && (!display || (!display->m_stopConfirmation && (display->m_settingsSelected || display->m_launcherVisible)))) return S_OK;
#else
					if (key == VK_ESCAPE) return S_OK;
#endif
					args->put_Handled(TRUE);
					if (SUCCEEDED(args->get_KeyEventKind(&kind)) && SUCCEEDED(args->get_PhysicalKeyStatus(&physical)) &&
						(kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN || kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) && !physical.WasKeyDown)
					{
#ifdef CEMU_NATIVE_HOST
						if (display && !notifications.m_closing) { const auto session = core->SessionId(); app->post([&, key, session] { display->Shortcut(key, session); }); }
#endif
					}
					return S_OK;
				});
			if (FAILED(notifications.m_controller->add_AcceleratorKeyPressed(accelerator.Get(), &acceleratorToken)))
				throw std::runtime_error("Unable to register launcher fullscreen shortcut");
			notifications.m_acceleratorListener = acceleratorToken;
			webview.on<saucer::webview::event::dom_ready>([&] {
				notifications.m_frontendReady = true;
				updateWindowState();
				publishPreferences();
#ifdef CEMU_NATIVE_HOST
				if (core) publishCore(core->StatusJson());
				else
#endif
					webview.execute("window.dispatchEvent(new CustomEvent('launcher-core-state', {detail:{phase:'unavailable',canLaunch:false}}));");
#ifdef CEMU_NATIVE_HOST
				if (display) display->Publish();
#endif
			});
#ifdef CEMU_NATIVE_HOST
			// File は OS が作った WebView2 object からだけ取得する。JS の path 文字列は受け付けない。
			notifications.m_nativeWebView = webview.native().webview;
			EventRegistrationToken dropToken{};
			const auto dropHandler = Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
				[&](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
					if (!core || !display || notifications.m_closing || !display->m_settingsSelected) return S_OK;
					PWSTR source = nullptr, message = nullptr;
					if (FAILED(args->get_Source(&source))) return S_OK;
					const bool embedded = std::wstring_view(source) == L"saucer://embedded/index.html"; CoTaskMemFree(source);
					if (!embedded || FAILED(args->TryGetWebMessageAsString(&message))) return S_OK;
					const std::wstring command(message); CoTaskMemFree(message);
					const std::wstring prefix = L"account:drop:";
					if (!command.starts_with(prefix)) return S_OK;
					const auto tail = command.substr(prefix.size());
					if (tail.empty() || tail.size() > 16 || tail.find_first_not_of(L"0123456789") != std::wstring::npos) return S_OK;
					uint64_t requestId = 0;
					for (auto digit : tail) requestId = requestId * 10 + digit - L'0';
					if (!requestId || requestId > 9007199254740991ULL) return S_OK;
					Microsoft::WRL::ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> extended;
					Microsoft::WRL::ComPtr<ICoreWebView2ObjectCollectionView> objects;
					Microsoft::WRL::ComPtr<IUnknown> object;
					Microsoft::WRL::ComPtr<ICoreWebView2File> file;
					UINT count = 0; PWSTR path = nullptr;
					if (FAILED(args->QueryInterface(IID_PPV_ARGS(&extended))) || FAILED(extended->get_AdditionalObjects(&objects)) ||
						FAILED(objects->get_Count(&count)) || count != 1 || FAILED(objects->GetValueAtIndex(0, &object)) ||
						FAILED(object.As(&file)) || FAILED(file->get_Path(&path))) return S_OK;
					std::wstring selected(path); CoTaskMemFree(path);
					app->post([&, requestId, selected] {
						if (notifications.m_closing) return;
						const auto snapshot = core->PreviewAccountZip(requestId, selected);
						webview.execute("window.dispatchEvent(new CustomEvent('launcher-account-result', {detail:{requestId:" + std::to_string(requestId) + ",account:" + snapshot + "}}));");
					});
					return S_OK;
				});
			if (FAILED(notifications.m_nativeWebView->add_WebMessageReceived(dropHandler.Get(), &dropToken))) throw std::runtime_error("Unable to register account drop");
			 notifications.m_dropListener = dropToken;
#endif
			webview.on<saucer::webview::event::message>([&](std::string_view message) {
				const auto url = webview.url();
				if (notifications.m_closing || url.scheme() != "saucer" || url.host() != "embedded" || url.path() != "/index.html")
					return saucer::status::unhandled;
#ifdef CEMU_NATIVE_HOST
				if (display && display->m_stopping && message != "window:close" && message != "window:close-confirmed" &&
					!message.starts_with("settings:save:") && !message.starts_with("preferences:save:") && !message.starts_with("av:command:"))
					return saucer::status::handled;
#endif

				// 埋め込みページからの完全一致するウィンドウ操作だけを受け付ける。
				if (message == "window:minimize")
					window->set_minimized(true);
				else if (message == "window:toggle-maximize")
					window->set_maximized(!window->maximized());
				else if (message == "window:close")
					window->close();
				else if (message == "window:close-confirmed" && notifications.m_closeRequested)
				{
					notifications.m_closeApproved = true;
					window->close();
				}
				else if (message.starts_with("preferences:save:"))
				{
					if (const auto request = ParseSaveRequest(message.substr(std::string_view("preferences:save:").size())))
					{
						LauncherPreferences edited;
						if (!LauncherPreferences::Parse(request->second, edited)) preferencesNotice = "invalid";
						else if (edited == preferences) preferencesNotice = "saved";
						else if (!edited.Save(preferencesPath)) preferencesNotice = "save-failed";
						else { preferences = edited; preferencesNotice = "saved"; }
						webview.execute("window.dispatchEvent(new CustomEvent('launcher-settings-result', {detail:{scope:'preferences',requestId:" +
							std::to_string(request->first) + ",values:" + preferences.Json() + ",notice:'" + preferencesNotice + "'}}));");
					}
				}
#ifdef CEMU_NATIVE_HOST
				else if (core && message == "game:start")
					core->LaunchGame();
				else if (core && message.starts_with("settings:save:"))
				{
					const auto payload = message.substr(std::string_view("settings:save:").size());
					if (const auto request = ParseSaveRequest(payload))
					{
						core->SaveNativeSettings(request->second);
						webview.execute("window.dispatchEvent(new CustomEvent('launcher-settings-result', {detail:{scope:'native',requestId:" +
							std::to_string(request->first) + ",state:" + core->StatusJson() + "}}));");
					}
					else core->SaveNativeSettings(payload);
				}
				else if (core && message.starts_with("account:select-zip:") && display->m_settingsSelected && !display->m_stopping)
				{
					if (const auto id = ParseStopId(message.substr(std::string_view("account:select-zip:").size())); id && *id <= 9007199254740991ULL)
					{
						app->post([&, requestId = *id] {
							if (notifications.m_closing || display->m_stopping) return;
							const auto selected = SelectDumplingZip(window->native().hwnd);
							const auto snapshot = selected ? core->PreviewAccountZip(requestId, *selected) : core->AccountCommand(requestId, "{\"op\":\"list\"}");
							webview.execute("window.dispatchEvent(new CustomEvent('launcher-account-result', {detail:{requestId:" + std::to_string(requestId) + ",account:" + snapshot + "}}));");
						});
					}
				}
				else if (core && message.starts_with("account:select-custom:") && display->m_settingsSelected && !display->m_stopping)
				{
					const auto tail = message.substr(std::string_view("account:select-custom:").size()); const auto colon = tail.find(':');
					if (colon != std::string_view::npos && (tail.substr(colon + 1) == "0" || tail.substr(colon + 1) == "1"))
						if (const auto id = ParseStopId(tail.substr(0, colon)); id && *id <= 9007199254740991ULL)
							app->post([&, requestId = *id, replace = tail.substr(colon + 1) == "1"] {
								if (notifications.m_closing || display->m_stopping) return;
								const auto selected = SelectDumplingZip(window->native().hwnd, true);
								const auto snapshot = selected ? core->ImportCustomNetwork(requestId, *selected, replace) : core->AccountCommand(requestId, "{\"op\":\"list\"}");
								webview.execute("window.dispatchEvent(new CustomEvent('launcher-account-result', {detail:{requestId:" + std::to_string(requestId) + ",account:" + snapshot + "}}));");
							});
				}
				else if (core && message.starts_with("account:command:"))
				{
					if (const auto request = ParseSaveRequest(message.substr(std::string_view("account:command:").size())))
					{
						const auto snapshot = core->AccountCommand(request->first, request->second);
						webview.execute("window.dispatchEvent(new CustomEvent('launcher-account-result', {detail:{requestId:" + std::to_string(request->first) + ",account:" + snapshot + "}}));");
						publishCore(core->StatusJson());
					}
				}
				else if (core && message.starts_with("av:command:"))
				{
					if (const auto request = ParseSaveRequest(message.substr(std::string_view("av:command:").size())))
					{
						const auto snapshot = core->AvCommand(request->first, request->second);
						webview.execute("window.dispatchEvent(new CustomEvent('launcher-av-result', {detail:{requestId:" + std::to_string(request->first) + ",settings:" + snapshot + "}}));");
						core->RefreshSettingsInfo();
					}
				}
				else if (core && message.starts_with("input:command:"))
				{
					if (const auto request = ParseSaveRequest(message.substr(std::string_view("input:command:").size())))
					{
						const auto snapshot = core->InputCommand(request->first, request->second);
						webview.execute("window.dispatchEvent(new CustomEvent('launcher-input-result', {detail:{requestId:" + std::to_string(request->first) + ",input:" + snapshot + "}}));");
						core->RefreshSettingsInfo();
					}
				}
				else if (core && message == "input:cancel")
					core->CancelInputCapture();
				else if (core && message.starts_with("input:poll:") && display->m_settingsSelected && !IsIconic(window->native().hwnd))
				{
					if (const auto id = ParseStopId(message.substr(std::string_view("input:poll:").size())); id && *id <= 9007199254740991ULL)
						webview.execute("window.dispatchEvent(new CustomEvent('launcher-input-live', {detail:{viewId:" + std::to_string(*id) + ",state:" + core->PollInput() + "}}));");
				}
				else if (core && message == "game:return")
					display->ReturnToGame();
				else if (core && (message == "friends:get" || message == "friends:refresh"))
				{
					if (display->m_stopping || core->IsTitleStopping()) return saucer::status::handled;
					const auto snapshot = core->FriendsCommand(message == "friends:refresh");
					webview.execute("window.dispatchEvent(new CustomEvent('launcher-friends-state', {detail:" + snapshot + "}));");
				}
				else if (core && message == "settings:get")
					core->RefreshSettingsInfo();
				else if (core && s_sessionDiagnostics && message == "game:stop")
					core->StopGame();
				else if (core && s_sessionDiagnostics && message == "game:diagnostic-display-failure")
					display->Fail();
				else if (core && message.starts_with("game:stop-request:"))
				{
					if (const auto session = ParseStopId(message.substr(std::string_view("game:stop-request:").size())))
						core->BeginStopGameConfirmation(*session);
				}
				else if (core && (message.starts_with("game:stop-confirm:") || message.starts_with("game:stop-cancel:")))
				{
					const bool confirm = message.starts_with("game:stop-confirm:");
					const auto prefix = confirm ? std::string_view("game:stop-confirm:") : std::string_view("game:stop-cancel:");
					if (const auto ids = ParseStopConfirmation(message.substr(prefix.size())))
					{
						if (confirm) core->ConfirmStopGame(ids->first, ids->second);
						else core->CancelStopGameConfirmation(ids->first, ids->second);
					}
				}
				else if (core && (message == "page:home" || message == "page:settings" || message == "page:friends"))
				{
					if (display->m_stopping || display->m_stopConfirmation || core->IsTitleStopping()) return saucer::status::handled;
					core->CancelInputCapture();
					if (message == "page:home" && core->IsRunning()) { display->ReturnToGame(); return saucer::status::handled; }
					display->m_settingsSelected = message != "page:home";
					if (display->m_settingsSelected) core->RefreshSettingsInfo();
					display->m_launcherVisible = true;
					if (display->m_transitioning || display->IsFullscreen()) return saucer::status::handled;
					const bool running = core->IsRunning();
					// HWND を隠しても物理寸法と GPU の Present/SwapBuffers 経路は保持する。
					// Launcher の入力 gate だけを閉じ、Core の実行状態は変更しない。
					core->InputMessage(WM_CANCELMODE, 0, 0);
					gameHost.SetVisible(false);
					if (running) webview.native().controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
				}
#endif
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
#ifdef CEMU_NATIVE_HOST
			if (core) core->Start(s_launchImmediately);
#endif
			co_await app->finish();
#ifdef CEMU_NATIVE_HOST
			gameHost.input = {};
			gameHost.stateChanged = {};
			gameHost.exitResult = {};
			gameHost.shortcut = {};
#endif
		} catch (const std::exception& error)
		{
			OutputDebugStringA(error.what());
			ReportStartupFailure();
			app->quit();
		}
	}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int)
{
	const std::wstring_view arguments(commandLine);
	if (arguments == L"--native-host-diagnostics" || arguments == L"--native-host-diagnostics-fail-after-create")
	{
		s_nativeHostDiagnostics = true;
		s_failHostInitialization = arguments == L"--native-host-diagnostics-fail-after-create";
	}
	else if (arguments == L"--gui-only")
		s_guiOnly = true;
#ifdef CEMU_NATIVE_HOST
	else if (arguments == L"--core-integration-test")
		s_launchImmediately = true;
	else if (arguments == L"--session-lifecycle-test")
		s_sessionDiagnostics = true;
	else if (arguments == L"--core-initialize-test")
		s_launchImmediately = false;
#endif
	else if (!arguments.empty())
		return 2;
	try
	{
		int result;
		{
			auto application = saucer::application::create({.id = "MCWiiU-Launcher"}).value();
			result = application.run(Start);
		}
		const auto exitCode = s_startupExitCode ? s_startupExitCode : result;
#ifdef CEMU_NATIVE_HOST
		// CemuApp::OnExit と同じ終了契約。Core/GPU/Input と全 UI の明示破棄後に
		// Cemu の process 寿命を持つ静的 thread の destructor を実行せず終了する。
		if (!s_guiOnly && !s_nativeHostDiagnostics) ExitProcess(exitCode);
#endif
		return exitCode;
	} catch (const std::exception& error)
	{
		OutputDebugStringA(error.what());
		ReportStartupFailure();
		return 1;
	}
}
