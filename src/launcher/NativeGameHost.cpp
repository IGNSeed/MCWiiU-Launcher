#include "NativeGameHost.h"

#include <WebView2.h>
#include <algorithm>
#include <exception>
#include <stdexcept>

namespace
{
	constexpr wchar_t s_className[] = L"MCWiiULauncher.NativeGameHost";
	constexpr UINT_PTR s_subclassId = 0x4d435748;
	// 診断起動時だけ受け付ける固定操作。外部 HWND やポインターは受け取らない。
	constexpr UINT s_diagnosticMessage = WM_APP + 0x340;
} // namespace

NativeGameHost::NativeGameHost(HWND parent, ICoreWebView2Controller* controller, bool diagnostics, bool failAfterCreate)
	: m_parent(parent), m_instance(GetModuleHandleW(nullptr)), m_controller(controller),
	  m_thread(GetCurrentThreadId()), m_diagnostics(diagnostics)
{
	DWORD process = 0;
	const auto thread = GetWindowThreadProcessId(parent, &process);
	APTTYPE apartment;
	APTTYPEQUALIFIER qualifier;
	if (!IsWindow(parent) || thread != m_thread || process != GetCurrentProcessId() || !controller ||
		FAILED(CoGetApartmentType(&apartment, &qualifier)) || (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA))
		throw std::runtime_error("Native game host requires its parent UI STA");

	try
	{
		WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW)};
		windowClass.lpfnWndProc = ChildProc;
		windowClass.hInstance = m_instance;
		windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		windowClass.lpszClassName = s_className;
		m_class = RegisterClassExW(&windowClass);
		if (!m_class)
			throw std::runtime_error("Unable to register native game host");
		// saucer の親は NOREDIRECTIONBITMAP。診断用 GDI だけ独立した描画面を持つ。
		// 通常の描画 HWND は layered にせず、将来の Vulkan surface 用に保持する。
		m_child = CreateWindowExW(diagnostics ? WS_EX_LAYERED : 0, s_className, L"Native host diagnostics - no game renderer",
			WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, nullptr, m_instance, this);
		if (!m_child)
			throw std::runtime_error("Unable to create native game host");
		if (diagnostics && !SetLayeredWindowAttributes(m_child, 0, 255, LWA_ALPHA))
			throw std::runtime_error("Unable to initialize diagnostic painting");
		m_subclassed = SetWindowSubclass(parent, ParentProc, s_subclassId, reinterpret_cast<DWORD_PTR>(this)) != FALSE;
		if (!m_subclassed || (diagnostics && failAfterCreate))
			throw std::runtime_error("Unable to attach native game host");
		UpdateLayout();
		if (m_layoutFailed)
			throw std::runtime_error("Unable to initialize native game host layout");
	} catch (...)
	{
		Release();
		throw;
	}
}

NativeGameHost::~NativeGameHost()
{
	Release();
}

bool NativeGameHost::OnUiThread() const
{
	return GetCurrentThreadId() == m_thread;
}

void NativeGameHost::Release()
{
	// 所有者は UI STA 内に固定する。破棄後に subclass から this を参照させない。
	if (!OnUiThread())
		std::terminate();
	if (m_subclassed)
		RemoveWindowSubclass(m_parent, ParentProc, s_subclassId);
	m_subclassed = false;
	if (m_child)
		DestroyWindow(m_child);
	if (m_diagnostics && m_parent && m_controller)
	{
		RECT client{};
		if (GetClientRect(m_parent, &client))
			m_controller->put_Bounds(client);
	}
	m_controller = nullptr;
	m_parent = nullptr;
	if (m_class)
		UnregisterClassW(s_className, m_instance);
	m_class = 0;
}

SIZE NativeGameHost::GetClientSize() const
{
	RECT client{};
	if (OnUiThread() && m_child)
		GetClientRect(m_child, &client);
	return {client.right, client.bottom};
}

bool NativeGameHost::IsRenderable() const
{
	const auto size = GetClientSize();
	return OnUiThread() && m_child && !m_layoutFailed && IsWindowVisible(m_child) &&
		!IsIconic(m_parent) && size.cx > 0 && size.cy > 0;
}

bool NativeGameHost::SetBounds(const RECT& bounds)
{
	RECT client{};
	if (!OnUiThread() || !m_child || !GetClientRect(m_parent, &client) || bounds.left < 0 || bounds.top < 0 ||
		bounds.right < bounds.left || bounds.bottom < bounds.top || bounds.right > client.right || bounds.bottom > client.bottom)
		return false;
	return SetWindowPos(m_child, HWND_TOP, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
		SWP_NOACTIVATE) != FALSE;
}

void NativeGameHost::RestoreWebViewFocus()
{
	if (m_child && GetFocus() == m_child && m_controller)
		m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void NativeGameHost::SetVisible(bool visible)
{
	if (!OnUiThread() || !m_child)
		return;
	if (!visible)
		RestoreWebViewFocus();
	m_visible = visible;
	UpdateLayout();
}

bool NativeGameHost::Focus()
{
	if (!OnUiThread() || !IsRenderable() || GetForegroundWindow() != m_parent)
		return false;
	SetFocus(m_child);
	return GetFocus() == m_child;
}

void NativeGameHost::UpdateLayout()
{
	if (!m_parent || !m_child)
		return;
	RECT client{};
	if (!GetClientRect(m_parent, &client))
		return;
	const bool active = m_visible && !IsIconic(m_parent) && IsWindowVisible(m_parent);
	if (m_diagnostics)
	{
		const auto dpi = GetDpiForWindow(m_parent);
		const LONG height = std::min<LONG>(client.bottom, MulDiv(160, dpi, 96));
		RECT webBounds = client;
		if (active)
			webBounds.bottom -= height;
		// saucer の resize 処理後に物理 client 座標で適用する。CSS の倍率とは独立。
		m_layoutFailed = FAILED(m_controller->put_Bounds(webBounds));
		const RECT gameBounds{std::min(client.right, static_cast<LONG>(MulDiv(210, dpi, 96))),
			client.bottom - height, client.right, client.bottom};
		m_layoutFailed = !SetBounds(gameBounds) || m_layoutFailed;
	}
	if (!active || m_layoutFailed)
		RestoreWebViewFocus();
	ShowWindow(m_child, active && !m_layoutFailed ? SW_SHOWNA : SW_HIDE);
}

LRESULT CALLBACK NativeGameHost::ParentProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
	auto* self = reinterpret_cast<NativeGameHost*>(data);
	if (message == WM_NCDESTROY)
	{
		RemoveWindowSubclass(hwnd, ParentProc, id);
		self->m_subclassed = false;
		self->m_parent = nullptr;
		self->m_controller = nullptr;
		return DefSubclassProc(hwnd, message, wParam, lParam);
	}
	if (message == s_diagnosticMessage && self->m_diagnostics && lParam == 0)
	{
		switch (wParam)
		{
		case 1:
			self->SetVisible(true);
			return 1;
		case 2:
			self->SetVisible(false);
			return 1;
		case 3:
			return self->Focus();
		default:
			return 0;
		}
	}
	const auto result = DefSubclassProc(hwnd, message, wParam, lParam);
	if (message == WM_SIZE || message == WM_DPICHANGED || message == WM_SHOWWINDOW)
		self->UpdateLayout();
	if (message == WM_MOVE && self->m_diagnostics && self->m_controller)
		self->m_controller->NotifyParentWindowPositionChanged();
	return result;
}

LRESULT CALLBACK NativeGameHost::ChildProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	auto* self = reinterpret_cast<NativeGameHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (message == WM_NCCREATE)
	{
		self = static_cast<NativeGameHost*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
	}
	if (!self)
		return DefWindowProcW(hwnd, message, wParam, lParam);
	switch (message)
	{
	case WM_NCDESTROY:
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
		self->m_child = nullptr;
		break;
	case WM_LBUTTONDOWN:
		self->Focus();
		return 0;
	case WM_KEYDOWN:
		if (wParam == VK_ESCAPE || wParam == VK_TAB)
			self->RestoreWebViewFocus();
		return 0;
	case WM_SYSKEYDOWN:
		if (wParam == VK_F4 && (lParam & (1LL << 29)))
		{
			PostMessageW(self->m_parent, WM_CLOSE, 0, 0);
			return 0;
		}
		break;
	case WM_SETFOCUS:
	case WM_KILLFOCUS:
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
	{
		PAINTSTRUCT paint;
		const auto dc = BeginPaint(hwnd, &paint);
		RECT client{};
		GetClientRect(hwnd, &client);
		FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
		if (self->m_diagnostics)
		{
			FrameRect(dc, &client, static_cast<HBRUSH>(GetStockObject(GetFocus() == hwnd ? WHITE_BRUSH : GRAY_BRUSH)));
			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, RGB(210, 210, 210));
			DrawTextW(dc, L"Native host diagnostics\nNo game renderer connected", -1, &client, DT_CENTER | DT_WORDBREAK);
		}
		EndPaint(hwnd, &paint);
		return 0;
	}
	case WM_CLOSE:
		return 0;
	}
	return DefWindowProcW(hwnd, message, wParam, lParam);
}
