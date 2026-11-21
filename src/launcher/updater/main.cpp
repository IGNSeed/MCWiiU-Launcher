#include "../UpdateHandoff.h"
#include <shellapi.h>
#include <thread>

namespace
{
	constexpr UINT StatusMessage = WM_APP + 1, CompleteMessage = WM_APP + 2;
	struct Context
	{
		std::atomic_uint applyGate{0};
		bool transaction = false;
		HWND text = nullptr, button = nullptr;
		int result = 1;
	};
	LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM key, LPARAM value)
	{
		auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(window, GWLP_USERDATA));
		if (message == WM_NCCREATE)
		{
			context = static_cast<Context*>(reinterpret_cast<CREATESTRUCTW*>(value)->lpCreateParams);
			SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
		}
		if (!context)
			return DefWindowProcW(window, message, key, value);
		if (message == WM_CREATE)
		{
			context->text = CreateWindowW(L"STATIC", L"Verifying the update handoff…", WS_CHILD | WS_VISIBLE, 24, 24, 430, 60, window, nullptr, nullptr, nullptr);
			context->button = CreateWindowW(L"BUTTON", L"Cancel Update", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 304, 100, 150, 32, window, reinterpret_cast<HMENU>(1), nullptr, nullptr);
			return 0;
		}
		if (message == StatusMessage)
		{
			const auto state = static_cast<LauncherUpdate::State>(key);
			context->transaction = state != LauncherUpdate::State::AwaitingShutdown;
			const wchar_t* text = L"Update recovery requires attention.";
			switch (state)
			{
			case LauncherUpdate::State::AwaitingShutdown:
				text = L"Waiting for the launcher to close normally…";
				break;
			case LauncherUpdate::State::Staging:
				text = L"Preparing the verified new version…";
				break;
			case LauncherUpdate::State::Activating:
				text = L"Activating the verified update…";
				break;
			case LauncherUpdate::State::VerifyingStartup:
				text = L"Starting the updated launcher…";
				break;
			case LauncherUpdate::State::Committing:
				text = L"Saving the verified startup result…";
				break;
			case LauncherUpdate::State::RollingBack:
				text = L"Restoring the verified previous version…";
				break;
			default:
				break;
			}
			SetWindowTextW(context->text, text);
			EnableWindow(context->button, !context->transaction);
			return 0;
		}
		if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(key) == 1))
		{
			unsigned expected = 0;
			if (context->applyGate.compare_exchange_strong(expected, 1))
			{
				EnableWindow(context->button, FALSE);
				SetWindowTextW(context->text, L"Cancelling before installation. Waiting for the launcher to close safely…");
			}
			return 0;
		}
		if (message == CompleteMessage)
		{
			context->result = static_cast<int>(key);
			DestroyWindow(window);
			return 0;
		}
		if (message == WM_DESTROY)
		{
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(window, message, key, value);
	}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
	int count = 0;
	auto** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
	if (!arguments || count != 5 || std::wstring_view(arguments[1]) != L"--handoff")
	{
		if (arguments)
			LocalFree(arguments);
		return 2;
	}
	const auto input = reinterpret_cast<HANDLE>(_wcstoui64(arguments[2], nullptr, 10));
	const auto parent = reinterpret_cast<HANDLE>(_wcstoui64(arguments[3], nullptr, 10));
	const auto acknowledgement = reinterpret_cast<HANDLE>(_wcstoui64(arguments[4], nullptr, 10));
	LocalFree(arguments);
	LauncherUpdate::File inputOwner(input), parentOwner(parent), acknowledgementOwner(acknowledgement);
	Context context;
	WNDCLASSW type{};
	type.hInstance = instance;
	type.lpfnWndProc = WindowProc;
	type.lpszClassName = L"MCWiiUUpdater";
	type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
	type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	if (!RegisterClassW(&type))
		return 1;
	const auto window = CreateWindowW(type.lpszClassName, L"MCWiiU Launcher Update", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 500, 190, nullptr, nullptr, instance, &context);
	if (!window)
		return 1;
	ShowWindow(window, SW_SHOW);
	UpdateWindow(window);
	std::thread worker([&] {
		const bool success = LauncherUpdate::RunUpdater(input, parent, acknowledgement, [window](LauncherUpdate::State state) { PostMessageW(window, StatusMessage, static_cast<WPARAM>(state), 0); }, &context.applyGate);
		PostMessageW(window, CompleteMessage, success ? 0 : 1, 0);
	});
	MSG message{};
	while (GetMessageW(&message, nullptr, 0, 0) > 0)
	{
		if (!IsDialogMessageW(window, &message))
		{
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}
	worker.join();
	return context.result;
}
