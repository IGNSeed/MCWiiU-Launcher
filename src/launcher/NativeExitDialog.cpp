#include "NativeExitDialog.h"

#include <CommCtrl.h>
#include <algorithm>
#include <stdexcept>

namespace
{
	constexpr wchar_t s_dialogClass[] = L"MCWiiULauncher.ExitConfirmation";
}

NativeExitDialog::NativeExitDialog(HWND parent) : m_parent(parent)
{
	WNDCLASSEXW wc{sizeof(wc)};
	wc.lpfnWndProc = WindowProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.lpszClassName = s_dialogClass;
	m_class = RegisterClassExW(&wc);
	if (!m_class) throw std::runtime_error("Unable to register exit confirmation");
}

NativeExitDialog::~NativeExitDialog()
{
	if (m_panel) DestroyWindow(m_panel);
	if (m_dim) DestroyWindow(m_dim);
	if (m_font) DeleteObject(m_font);
	if (m_headingFont) DeleteObject(m_headingFont);
	if (m_class) UnregisterClassW(s_dialogClass, GetModuleHandleW(nullptr));
}

void NativeExitDialog::Show(bool visible, const RECT& gameBounds)
{
	if (!visible)
	{
		m_visible = false;
		if (m_panel) ShowWindow(m_panel, SW_HIDE);
		if (m_dim) ShowWindow(m_dim, SW_HIDE);
		return;
	}
	if (!m_dim)
	{
		m_dim = CreateWindowExW(WS_EX_LAYERED, s_dialogClass, L"Minecraft confirmation backdrop", WS_CHILD | WS_CLIPSIBLINGS,
			0, 0, 1, 1, m_parent, nullptr, GetModuleHandleW(nullptr), this);
		m_panel = CreateWindowExW(WS_EX_LAYERED | WS_EX_CONTROLPARENT, s_dialogClass, L"Minecraftを終了しますか？",
			WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 1, 1, m_parent, nullptr, GetModuleHandleW(nullptr), this);
		if (!m_dim || !m_panel || !SetLayeredWindowAttributes(m_dim, 0, 170, LWA_ALPHA) ||
			!SetLayeredWindowAttributes(m_panel, 0, 255, LWA_ALPHA)) throw std::runtime_error("Unable to create exit confirmation");
		const wchar_t* labels[]{L"キャンセル", L"ゲームを終了"};
		for (unsigned i = 0; i < 2; ++i)
		{
			m_buttons[i] = CreateWindowExW(0, L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
				0, 0, 1, 1, m_panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(200 + i)), GetModuleHandleW(nullptr), nullptr);
			if (!m_buttons[i] || !SetWindowSubclass(m_buttons[i], ButtonProc, 1, reinterpret_cast<DWORD_PTR>(this)))
				throw std::runtime_error("Unable to create confirmation buttons");
		}
	}
	m_visible = true;
	m_pending = false;
	for (auto button : m_buttons) EnableWindow(button, TRUE);
	Layout(gameBounds);
	ShowWindow(m_dim, SW_SHOWNA);
	ShowWindow(m_panel, SW_SHOWNA);
	SetFocus(m_buttons[0]);
}

void NativeExitDialog::Layout(const RECT& bounds)
{
	if (!m_visible || !m_dim || !m_panel) return;
	const UINT dpi = GetDpiForWindow(m_parent);
	const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
	if (dpi != m_dpi)
	{
		if (m_font) DeleteObject(m_font);
		if (m_headingFont) DeleteObject(m_headingFont);
		m_font = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
		m_headingFont = CreateFontW(-px(20), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
		m_dpi = dpi;
		for (auto button : m_buttons) SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
	}
	const int width = std::min<LONG>(px(520), bounds.right - bounds.left);
	const int height = std::min<LONG>(px(280), bounds.bottom - bounds.top);
	SetWindowPos(m_dim, HWND_TOP, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOACTIVATE);
	SetWindowPos(m_panel, HWND_TOP, bounds.left + (bounds.right - bounds.left - width) / 2,
		bounds.top + (bounds.bottom - bounds.top - height) / 2, width, height, SWP_NOACTIVATE);
	const int buttonWidth = std::min(px(160), (width - px(60)) / 2);
	for (unsigned i = 0; i < 2; ++i)
		SetWindowPos(m_buttons[i], nullptr, width - px(24) - (2 - i) * buttonWidth - (1 - i) * px(12),
			height - px(68), buttonWidth, px(42), SWP_NOACTIVATE | SWP_NOZORDER);
	InvalidateRect(m_panel, nullptr, FALSE);
}

void NativeExitDialog::Choose(bool confirm)
{
	if (!m_visible || m_pending) return;
	m_pending = true;
	for (auto button : m_buttons) EnableWindow(button, FALSE);
	if (result) result(confirm);
}

void NativeExitDialog::Paint(HWND hwnd)
{
	PAINTSTRUCT paint{};
	HDC dc = BeginPaint(hwnd, &paint);
	const int saved = SaveDC(dc);
	RECT rect{}; GetClientRect(hwnd, &rect);
	SetDCBrushColor(dc, hwnd == m_dim ? RGB(0, 0, 0) : RGB(20, 20, 23));
	FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
	if (hwnd == m_panel)
	{
		FrameRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
		const auto px = [this](int v) { return MulDiv(v, m_dpi, 96); };
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(245, 245, 245));
		SelectObject(dc, m_headingFont);
		RECT heading{px(24), px(24), rect.right - px(24), px(62)};
		DrawTextW(dc, L"Minecraftを終了しますか？", -1, &heading, DT_LEFT | DT_WORDBREAK);
		SelectObject(dc, m_font);
		SetTextColor(dc, RGB(192, 192, 200));
		RECT text{px(24), px(78), rect.right - px(24), rect.bottom - px(80)};
		DrawTextW(dc, L"保存していない変更は失われる可能性があります。\n先に Minecraft 内で「Exit and save」を実行してください。", -1, &text, DT_LEFT | DT_WORDBREAK);
	}
	RestoreDC(dc, saved);
	EndPaint(hwnd, &paint);
}

LRESULT CALLBACK NativeExitDialog::ButtonProc(HWND hwnd, UINT msg, WPARAM key, LPARAM data, UINT_PTR id, DWORD_PTR owner)
{
	auto* self = reinterpret_cast<NativeExitDialog*>(owner);
	if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
	{
		if (key == VK_ESCAPE || key == VK_F10 || key == VK_F11 || key == VK_TAB || key == VK_RETURN)
		{
			if (!(data & (1LL << 30)))
			{
				if (key == VK_ESCAPE) self->Choose(false);
				else if (key == VK_RETURN) self->Choose(hwnd == self->m_buttons[1]);
				else if (key == VK_TAB) SetFocus(hwnd == self->m_buttons[0] ? self->m_buttons[1] : self->m_buttons[0]);
			}
			return 0;
		}
	}
	if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && (key == VK_ESCAPE || key == VK_F10 || key == VK_F11 || key == VK_TAB || key == VK_RETURN)) return 0;
	if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, ButtonProc, id);
	return DefSubclassProc(hwnd, msg, key, data);
}

LRESULT CALLBACK NativeExitDialog::WindowProc(HWND hwnd, UINT msg, WPARAM key, LPARAM data)
{
	if (msg == WM_NCCREATE) SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(data)->lpCreateParams));
	auto* self = reinterpret_cast<NativeExitDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (!self) return DefWindowProcW(hwnd, msg, key, data);
	if (msg == WM_ERASEBKGND) return 1;
	if (msg == WM_PAINT) { self->Paint(hwnd); return 0; }
	if (msg == WM_COMMAND && HIWORD(key) == BN_CLICKED)
	{
		if (reinterpret_cast<HWND>(data) == self->m_buttons[0]) self->Choose(false);
		if (reinterpret_cast<HWND>(data) == self->m_buttons[1]) self->Choose(true);
		return 0;
	}
	if (msg == WM_DRAWITEM)
	{
		auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(data);
		const int saved = SaveDC(item->hDC);
		SetDCBrushColor(item->hDC, (item->itemState & ODS_SELECTED) ? RGB(75, 75, 80) : RGB(35, 35, 40));
		FillRect(item->hDC, &item->rcItem, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
		FrameRect(item->hDC, &item->rcItem, static_cast<HBRUSH>(GetStockObject((item->itemState & ODS_FOCUS) ? WHITE_BRUSH : GRAY_BRUSH)));
		SetBkMode(item->hDC, TRANSPARENT);
		SetTextColor(item->hDC, RGB(240, 240, 240));
		SelectObject(item->hDC, self->m_font);
		wchar_t label[64]{}; GetWindowTextW(item->hwndItem, label, 64);
		DrawTextW(item->hDC, label, -1, &item->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		RestoreDC(item->hDC, saved);
		return TRUE;
	}
	if (msg == WM_SETFOCUS && self->m_visible && !self->m_pending) { SetFocus(self->m_buttons[0]); return 0; }
	if (msg == WM_NCDESTROY) SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
	return DefWindowProcW(hwnd, msg, key, data);
}
