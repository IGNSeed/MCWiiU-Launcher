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
		INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_WIN95_CLASSES};
		if (!InitCommonControlsEx(&controls))
			throw std::runtime_error("Unable to initialize game controls");
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
		m_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP,
			CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, m_child, nullptr, m_instance, nullptr);
		if (!m_tooltip) throw std::runtime_error("Unable to create game control tooltips");
		const wchar_t* labels[]{L"Launcherへ戻る (F10)", L"フルスクリーン (F11)"};
		for (unsigned index = 0; index < 2; ++index)
		{
			// Vulkan の非redirect描画面上でも GDI 記号が見えるよう、この小領域だけに描画面を持たせる。
			m_controls[index] = CreateWindowExW(WS_EX_LAYERED, L"BUTTON", labels[index], WS_CHILD | BS_OWNERDRAW | BS_NOTIFY,
				0, 0, 0, 0, m_child, reinterpret_cast<HMENU>(static_cast<INT_PTR>(100 + index)), m_instance, nullptr);
			if (!m_controls[index] || !SetLayeredWindowAttributes(m_controls[index], 0, 245, LWA_ALPHA) ||
				!SetWindowSubclass(m_controls[index], ControlProc, s_subclassId, reinterpret_cast<DWORD_PTR>(this)))
				throw std::runtime_error("Unable to create game controls");
			// 既存 manifest の Common Controls v5 でも受け付ける構造体サイズ。
			TOOLINFOW tool{TTTOOLINFOW_V2_SIZE};
			tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
			tool.hwnd = m_child;
			tool.uId = reinterpret_cast<UINT_PTR>(m_controls[index]);
			tool.lpszText = const_cast<wchar_t*>(labels[index]);
			if (!SendMessageW(m_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool)))
				throw std::runtime_error("Unable to attach game control tooltips");
		}
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
	if (m_tooltip) DestroyWindow(m_tooltip);
	m_tooltip = nullptr;
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

void NativeGameHost::SetGameLayout(bool enabled)
{
	if (!OnUiThread()) return;
	m_gameLayout = enabled;
	UpdateLayout();
}

bool NativeGameHost::SetFullscreenLayout(bool enabled)
{
	if (!OnUiThread()) return false;
	m_fullscreenLayout = enabled;
	UpdateLayout();
	return !m_layoutFailed;
}

void NativeGameHost::SetControlsVisible(bool visible)
{
	if (!OnUiThread()) return;
	m_controlsVisible = visible;
	UpdateControls();
}

void NativeGameHost::UpdateControls()
{
	if (!m_child) return;
	const bool visible = m_controlsVisible && m_gameLayout && !m_fullscreenLayout && IsRenderable();
	const auto dpi = GetDpiForWindow(m_child);
	const auto size = MulDiv(32, dpi, 96), gap = MulDiv(6, dpi, 96), margin = MulDiv(12, dpi, 96);
	const auto client = GetClientSize();
	for (unsigned index = 0; index < 2; ++index)
	{
		if (!m_controls[index]) continue;
		if (visible)
			SetWindowPos(m_controls[index], HWND_TOP, client.cx - margin - size * (2 - index) - gap * (1 - index),
				margin, size, size, SWP_NOACTIVATE);
		if (bool(GetWindowLongPtrW(m_controls[index], GWL_STYLE) & WS_VISIBLE) != visible)
			ShowWindow(m_controls[index], visible ? SW_SHOWNA : SW_HIDE);
	}
	if (!visible && m_tooltip) SendMessageW(m_tooltip, TTM_POP, 0, 0);
}

void NativeGameHost::UpdateLayout()
{
	if (!m_parent || !m_child)
		return;
	RECT client{};
	if (!GetClientRect(m_parent, &client))
		return;
	const bool active = m_visible && !IsIconic(m_parent) && IsWindowVisible(m_parent);
	// saucer は最小化解除時に Controller を再表示するため、その後で抑止する。
	if (m_fullscreenLayout && FAILED(m_controller->put_IsVisible(FALSE)))
	{
		m_layoutFailed = true;
		if (stateChanged) stateChanged();
		return;
	}
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
	else if (m_gameLayout && !IsIconic(m_parent) && client.right > 0 && client.bottom > 0)
	{
		const auto dpi = GetDpiForWindow(m_parent);
		RECT bounds = client;
		if (!m_fullscreenLayout)
		{
			const LONG sidebar = std::min(client.right - 1, static_cast<LONG>(MulDiv(210, dpi, 96)));
			const LONG top = std::min(client.bottom - 1, static_cast<LONG>(MulDiv(48, dpi, 96)));
			const LONG inset = std::max<LONG>(0, std::min({static_cast<LONG>(MulDiv(24, dpi, 96)), (client.right - sidebar - 1) / 2, (client.bottom - top - 1) / 2}));
			bounds = {sidebar + inset, top + inset, client.right - inset, client.bottom - inset};
		}
		m_layoutFailed = !SetBounds(bounds);
	}
	if (!active || m_layoutFailed)
		RestoreWebViewFocus();
	ShowWindow(m_child, active && !m_layoutFailed ? SW_SHOWNA : SW_HIDE);
	UpdateControls();
	if (stateChanged) stateChanged();
}

LRESULT CALLBACK NativeGameHost::ControlProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
	auto* self = reinterpret_cast<NativeGameHost*>(data);
	if (message == WM_MOUSEMOVE && self->m_hoveredControl != hwnd)
	{
		self->m_hoveredControl = hwnd;
		TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
		TrackMouseEvent(&track);
		InvalidateRect(hwnd, nullptr, FALSE);
	}
	if (message == WM_MOUSELEAVE)
	{
		if (self->m_hoveredControl == hwnd) self->m_hoveredControl = nullptr;
		InvalidateRect(hwnd, nullptr, FALSE);
	}
	if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, ControlProc, id);
	return DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK NativeGameHost::ParentProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
	auto* self = reinterpret_cast<NativeGameHost*>(data);
	// saucer partial decoration の Windows 11 用上端1pxを全画面時だけ除く。
	if (message == WM_NCCALCSIZE && wParam && self->m_fullscreenLayout)
		return 0;
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
	if (message == WM_ACTIVATEAPP && !wParam && self->input)
		self->input(WM_KILLFOCUS, 0, 0);
	if (message == WM_ACTIVATEAPP && self->stateChanged)
		self->stateChanged();
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
	case WM_COMMAND:
		if (HIWORD(wParam) == BN_CLICKED && (reinterpret_cast<HWND>(lParam) == self->m_controls[0] || reinterpret_cast<HWND>(lParam) == self->m_controls[1]))
		{
			if (self->m_controlsVisible && !self->m_fullscreenLayout && self->IsRenderable())
			{
				// キー操作と同じ game focus を保存し、F11 解除後も入力を復帰する。
				self->Focus();
				if (self->input) self->input(WM_CANCELMODE, 0, 0);
				if (self->shortcut) self->shortcut(reinterpret_cast<HWND>(lParam) == self->m_controls[0] ? VK_F10 : VK_F11);
			}
			return 0;
		}
		break;
	case WM_DRAWITEM:
	{
		const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
		if (item->hwndItem != self->m_controls[0] && item->hwndItem != self->m_controls[1]) break;
		// stock object と DC 内の色だけを使い、再Playごとの GDI allocation を避ける。
		const auto saved = SaveDC(item->hDC);
		const bool hovered = self->m_hoveredControl == item->hwndItem;
		SetDCBrushColor(item->hDC, hovered ? RGB(48, 48, 54) : RGB(24, 24, 28));
		SelectObject(item->hDC, GetStockObject(DC_BRUSH));
		const auto r = item->rcItem;
		// 全画素を塗ってから矩形の枠を描き、layered 面の四隅を未描画にしない。
		FillRect(item->hDC, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
		SetDCBrushColor(item->hDC, hovered ? RGB(130, 130, 140) : RGB(90, 90, 98));
		FrameRect(item->hDC, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
		SetDCBrushColor(item->hDC, RGB(225, 225, 232));
		const LONG width = r.right - r.left, height = r.bottom - r.top;
		const LONG stroke = std::max<LONG>(2, width / 12), glyph = std::max<LONG>(8, width / 2);
		const LONG left = r.left + (width - glyph) / 2, top = r.top + (height - glyph) / 2;
		const auto fill = [&](LONG x, LONG y, LONG w, LONG h) {
			const RECT part{x, y, x + w, y + h};
			FillRect(item->hDC, &part, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
		};
		if (item->hwndItem == self->m_controls[0])
		{
			fill(left, top, stroke, glyph);
			fill(left + glyph - stroke, top, stroke, glyph);
		}
		else
		{
			const LONG arm = std::max<LONG>(stroke + 1, glyph / 3);
			for (const LONG x : {left, left + glyph - arm}) for (const LONG y : {top, top + glyph - stroke}) fill(x, y, arm, stroke);
			for (const LONG x : {left, left + glyph - stroke}) for (const LONG y : {top, top + glyph - arm}) fill(x, y, stroke, arm);
		}
		if (item->itemState & ODS_FOCUS) DrawFocusRect(item->hDC, &item->rcItem);
		RestoreDC(item->hDC, saved);
		return TRUE;
	}
	case WM_NCDESTROY:
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
		self->m_child = nullptr;
		break;
	case WM_LBUTTONDOWN:
		self->Focus();
		return 0;
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (wParam == VK_F11 || (wParam == VK_F10 && !(lParam & (1LL << 29))))
		{
			// 両側の down/up を消費し、フォーカス変更後のリピートも再配送しない。
			if (!(lParam & (1LL << 30)))
			{
				if (self->shortcut) self->shortcut(static_cast<UINT>(wParam));
				else if (wParam == VK_F10) self->RestoreWebViewFocus();
			}
			return 0;
		}
		if (self->m_diagnostics && (wParam == VK_ESCAPE || wParam == VK_TAB))
		{
			self->RestoreWebViewFocus();
			return 0;
		}
		if (wParam == VK_F4 && (lParam & (1LL << 29)))
		{
			PostMessageW(self->m_parent, WM_CLOSE, 0, 0);
			return 0;
		}
		if (self->input) self->input(message, wParam, lParam);
		return message == WM_SYSKEYDOWN ? DefWindowProcW(hwnd, message, wParam, lParam) : 0;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		if (wParam == VK_F10 || wParam == VK_F11) return 0;
		if (self->input) self->input(message, wParam, lParam);
		return 0;
	case WM_SETFOCUS:
	case WM_KILLFOCUS:
	case WM_CANCELMODE:
		if (self->input) self->input(message, wParam, lParam);
		if (self->m_diagnostics) InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
	{
		PAINTSTRUCT paint;
		const auto dc = BeginPaint(hwnd, &paint);
		RECT client{};
		GetClientRect(hwnd, &client);
		if (self->m_diagnostics)
		{
			FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
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
