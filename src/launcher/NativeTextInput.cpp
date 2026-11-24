#include "NativeTextInput.h"

#include <imm.h>
#include <windowsx.h>
#include <algorithm>
#include <stdexcept>

namespace
{
	constexpr wchar_t s_class[] = L"MCWiiULauncher.TextInput";
	RECT InputFieldBounds(LONG width, LONG height, UINT dpi)
	{
		const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
		const LONG margin = std::min<LONG>(px(24), width / 12);
		const LONG top = std::min<LONG>(px(65), height / 3);
		const LONG fieldHeight = std::max<LONG>(1, std::min<LONG>(px(38), height / 4));
		return {margin, top, width - margin, top + fieldHeight};
	}
	void Position(HWND window, HWND after, const RECT& bounds, UINT flags)
	{
		RECT previous{};
		GetWindowRect(window, &previous);
		MapWindowPoints(nullptr, GetParent(window), reinterpret_cast<POINT*>(&previous), 2);
		if (EqualRect(&previous, &bounds) && (flags & SWP_NOZORDER)) return;
		SetWindowPos(window, after, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, flags | SWP_NOACTIVATE);
	}
}

NativeTextInput::NativeTextInput(HWND parent) : m_parent(parent)
{
	WNDCLASSEXW wc{sizeof(wc)};
	wc.lpfnWndProc = WindowProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.lpszClassName = s_class;
	m_class = RegisterClassExW(&wc);
	if (!m_class) throw std::runtime_error("Unable to register text input");
	m_background = CreateSolidBrush(RGB(12, 12, 12));
	if (!m_background) { UnregisterClassW(s_class, wc.hInstance); throw std::runtime_error("Unable to create text input brush"); }
}

NativeTextInput::~NativeTextInput()
{
	if (m_panel) DestroyWindow(m_panel);
	if (m_dim) DestroyWindow(m_dim);
	if (m_font) DeleteObject(m_font);
	if (m_background) DeleteObject(m_background);
	if (m_class) UnregisterClassW(s_class, GetModuleHandleW(nullptr));
}

void NativeTextInput::Create()
{
	const auto instance = GetModuleHandleW(nullptr);
	m_dim = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, s_class, L"", WS_CHILD | WS_CLIPSIBLINGS,
		0, 0, 1, 1, m_parent, nullptr, instance, this);
	m_panel = CreateWindowExW(WS_EX_LAYERED | WS_EX_CONTROLPARENT, s_class, L"テキスト入力", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
		0, 0, 1, 1, m_parent, nullptr, instance, this);
	if (!m_dim || !m_panel || !SetLayeredWindowAttributes(m_dim, 0, 170, LWA_ALPHA) || !SetLayeredWindowAttributes(m_panel, 0, 255, LWA_ALPHA))
		throw std::runtime_error("Unable to create text input panel");
	// EDIT が IME の未確定文字・候補・Unicode 編集を処理する。ANSI 変換は行わない。
	m_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_CENTER,
		0, 0, 1, 1, m_panel, reinterpret_cast<HMENU>(300), instance, nullptr);
	if (!m_edit || !SetWindowSubclass(m_edit, InputProc, 1, reinterpret_cast<DWORD_PTR>(this)))
		throw std::runtime_error("Unable to create Unicode text input");
	const wchar_t* labels[]{L"キャンセル", L"確定"};
	for (unsigned i = 0; i < 2; ++i)
	{
		m_buttons[i] = CreateWindowExW(0, L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
			0, 0, 1, 1, m_panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(301 + i)), instance, nullptr);
		if (!m_buttons[i] || !SetWindowSubclass(m_buttons[i], InputProc, 1, reinterpret_cast<DWORD_PTR>(this)))
			throw std::runtime_error("Unable to create text input buttons");
	}
}

bool NativeTextInput::HasFocus() const
{
	const auto focus = GetFocus();
	return m_panel && (focus == m_panel || IsChild(m_panel, focus));
}

bool NativeTextInput::Focus()
{
	if (!m_visible || m_pending || GetForegroundWindow() != m_parent) return false;
	SetFocus(m_edit);
	return GetFocus() == m_edit;
}

std::u16string NativeTextInput::Text() const
{
	const int length = GetWindowTextLengthW(m_edit);
	std::wstring text(static_cast<size_t>(length) + 1, L'\0');
	text.resize(GetWindowTextW(m_edit, text.data(), static_cast<int>(text.size())));
	return std::u16string(text.begin(), text.end());
}

void NativeTextInput::Sync(bool allowed, const RECT& bounds)
{
	const auto state = read ? read() : swkbd::host::Snapshot{};
	if (!allowed || !state.active)
	{
		if (!m_visible) return;
		const bool focused = HasFocus();
		m_visible = false;
		// 隠れたセッションに未確定の IME 文字を残さない。
		if (const auto context = ImmGetContext(m_edit))
		{
			ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
			ImmReleaseContext(m_edit, context);
		}
		ShowWindow(m_panel, SW_HIDE);
		ShowWindow(m_dim, SW_HIDE);
		m_composing = m_imeKey = false;
		if (focused && restoreFocus) restoreFocus();
		return;
	}
	if (!m_panel) Create();
	// 初期文字列の caret を設定する前に、1px の作成時サイズを実寸へ直す。
	Layout(bounds);
	const bool fresh = state.generation != m_generation;
	const bool show = !m_visible;
	if (fresh)
	{
		m_generation = state.generation;
		m_pending = m_composing = m_imeKey = false;
		m_limit = state.limit;
		// EM_SETLIMITTEXT の 0 は無制限なので、実際の上限は Guest 側でも検証する。
		SendMessageW(m_edit, EM_SETLIMITTEXT, std::max(1u, m_limit), 0);
		for (const auto child : {m_edit, m_buttons[0], m_buttons[1]}) EnableWindow(child, TRUE);
	}
	if (!m_composing && (fresh || (state.revision != m_revision && !state.pending && !m_pending)))
	{
		m_revision = state.revision;
		if (fresh || Text() != state.text)
		{
			m_updating = true;
			const std::wstring text(state.text.begin(), state.text.end());
			SetWindowTextW(m_edit, text.c_str());
			SendMessageW(m_edit, EM_SETSEL, fresh ? state.cursor : text.size(), fresh ? state.cursor : text.size());
			m_updating = false;
		}
	}
	m_visible = true;
	Layout(bounds);
	if (show)
	{
		SetWindowPos(m_dim, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
		SetWindowPos(m_panel, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
	}
	if (show || fresh) Focus();
}

void NativeTextInput::Layout(const RECT& bounds)
{
	const auto dpi = GetDpiForWindow(m_parent);
	if (EqualRect(&m_bounds, &bounds) && m_dpi == dpi) return;
	m_bounds = bounds;
	const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
	if (dpi != m_dpi)
	{
		// Windows に既存の日本語 UI フォントと GDI font linking を使用する。
		const auto font = CreateFontW(-px(18), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
		if (font)
		{
			for (const auto child : {m_edit, m_buttons[0], m_buttons[1]}) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
			if (m_font) DeleteObject(m_font);
			m_font = font;
		}
		m_dpi = dpi;
	}
	const LONG availableWidth = std::max<LONG>(1, bounds.right - bounds.left);
	const LONG availableHeight = std::max<LONG>(1, bounds.bottom - bounds.top);
	const LONG width = std::min<LONG>(px(560), availableWidth), height = std::min<LONG>(px(210), availableHeight);
	Position(m_dim, nullptr, bounds, SWP_NOZORDER);
	const RECT panel{bounds.left + (availableWidth - width) / 2, bounds.top + (availableHeight - height) / 2,
		bounds.left + (availableWidth + width) / 2, bounds.top + (availableHeight + height) / 2};
	Position(m_panel, nullptr, panel, SWP_NOZORDER);
	const LONG margin = std::min<LONG>(px(24), width / 12);
	const auto field = InputFieldBounds(width, height, dpi);
	// 単一行 EDIT は上詰めなので、実フォントの行高で内側の HWND を上下中央へ置く。
	TEXTMETRICW metrics{};
	if (const auto dc = GetDC(m_edit))
	{
		const auto previous = SelectObject(dc, m_font ? m_font : GetStockObject(DEFAULT_GUI_FONT));
		GetTextMetricsW(dc, &metrics);
		SelectObject(dc, previous);
		ReleaseDC(m_edit, dc);
	}
	const LONG textHeight = std::min(field.bottom - field.top, std::max<LONG>(1, metrics.tmHeight));
	const LONG textTop = field.top + (field.bottom - field.top - textHeight) / 2;
	const LONG horizontalPadding = std::min<LONG>(px(8), (field.right - field.left) / 8);
	Position(m_edit, nullptr, {field.left + horizontalPadding, textTop, field.right - horizontalPadding, textTop + textHeight}, SWP_NOZORDER);
	const LONG buttonWidth = std::max<LONG>(1, std::min<LONG>(px(120), (width - margin * 3) / 2));
	const LONG buttonHeight = std::max<LONG>(1, std::min<LONG>(px(38), height / 4));
	for (unsigned i = 0; i < 2; ++i)
	{
		const LONG left = width - margin - (2 - i) * buttonWidth - (1 - i) * margin;
		Position(m_buttons[i], nullptr, {left, height - margin - buttonHeight, left + buttonWidth, height - margin}, SWP_NOZORDER);
	}
	InvalidateRect(m_panel, nullptr, FALSE);
}

void NativeTextInput::Changed()
{
	if (!m_visible || m_updating || m_composing || m_pending || !submit) return;
	const auto raw = Text();
	// WM_CHAR はサロゲートを二回に分けるため、high surrogate の途中では確定しない。
	if (!raw.empty() && raw.back() >= 0xd800 && raw.back() <= 0xdbff && raw.size() < m_limit) return;
	const auto text = swkbd::host::Sanitize(raw, m_limit);
	if (raw != text)
	{
		DWORD first = 0, last = 0;
		SendMessageW(m_edit, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
		m_updating = true;
		const std::wstring clean(text.begin(), text.end());
		SetWindowTextW(m_edit, clean.c_str());
		SendMessageW(m_edit, EM_SETSEL, std::min<size_t>(first, text.size()), std::min<size_t>(last, text.size()));
		m_updating = false;
	}
	submit(m_generation, text, swkbd::host::Action::Change);
}

void NativeTextInput::Choose(swkbd::host::Action action)
{
	if (!m_visible || m_composing || m_pending || !submit) return;
	if (!submit(m_generation, Text(), action)) return;
	m_pending = true;
	for (const auto child : {m_edit, m_buttons[0], m_buttons[1]}) EnableWindow(child, FALSE);
}

void NativeTextInput::Paint(HWND window)
{
	PAINTSTRUCT paint{};
	const auto dc = BeginPaint(window, &paint);
	const auto saved = SaveDC(dc);
	RECT bounds{};
	GetClientRect(window, &bounds);
	SetDCBrushColor(dc, window == m_dim ? RGB(0, 0, 0) : RGB(12, 12, 12));
	FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
	if (window == m_panel)
	{
		FrameRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
		const auto px = [this](int value) { return MulDiv(value, m_dpi, 96); };
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(245, 245, 245));
		SelectObject(dc, m_font);
		RECT title{std::min<LONG>(px(24), bounds.right / 12), px(18), bounds.right - px(12), std::min<LONG>(px(52), bounds.bottom / 3)};
		DrawTextW(dc, L"テキスト入力", -1, &title, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
		auto field = InputFieldBounds(bounds.right, bounds.bottom, m_dpi);
		InflateRect(&field, 1, 1);
		FrameRect(dc, &field, static_cast<HBRUSH>(GetStockObject(GetFocus() == m_edit ? WHITE_BRUSH : GRAY_BRUSH)));
	}
	RestoreDC(dc, saved);
	EndPaint(window, &paint);
}

LRESULT CALLBACK NativeTextInput::InputProc(HWND window, UINT message, WPARAM key, LPARAM value, UINT_PTR id, DWORD_PTR owner)
{
	auto* self = reinterpret_cast<NativeTextInput*>(owner);
	if (message == WM_NCDESTROY) RemoveWindowSubclass(window, InputProc, id);
	if (window == self->m_edit)
	{
		if (message == WM_IME_STARTCOMPOSITION) self->m_composing = true;
		if (message == WM_IME_COMPOSITION) self->m_composing = true;
		if (message == WM_IME_ENDCOMPOSITION)
		{
			const auto result = DefSubclassProc(window, message, key, value);
			self->m_composing = false;
			self->Changed();
			return result;
		}
	}
	if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
	{
		if (key == VK_F10 || key == VK_F11)
		{
			if (!(value & (1LL << 30)) && self->shortcut) self->shortcut(static_cast<UINT>(key));
			return 0;
		}
		if (key == VK_F4 && (value & (1LL << 29))) { PostMessageW(self->m_parent, WM_CLOSE, 0, 0); return 0; }
		if (key == VK_RETURN || key == VK_ESCAPE)
		{
			if (self->m_composing) { self->m_imeKey = true; return DefSubclassProc(window, message, key, value); }
			if (!(value & (1LL << 30)) && !self->m_imeKey)
				self->Choose(key == VK_ESCAPE || window == self->m_buttons[0] ? swkbd::host::Action::Cancel : swkbd::host::Action::Accept);
			return 0;
		}
		if (key == 'A' && (GetKeyState(VK_CONTROL) & 0x8000) && window == self->m_edit) { SendMessageW(window, EM_SETSEL, 0, -1); return 0; }
		if (key == VK_TAB && !self->m_composing)
		{
			const HWND controls[]{self->m_edit, self->m_buttons[0], self->m_buttons[1]};
			for (unsigned i = 0; i < 3; ++i) if (window == controls[i]) SetFocus(controls[(i + ((GetKeyState(VK_SHIFT) & 0x8000) ? 2 : 1)) % 3]);
			return 0;
		}
	}
	if ((message == WM_KEYUP || message == WM_SYSKEYUP) && (key == VK_RETURN || key == VK_ESCAPE)) { self->m_imeKey = false; return 0; }
	if ((message == WM_KEYUP || message == WM_SYSKEYUP) && (key == VK_F10 || key == VK_F11)) return 0;
	if (message == WM_CHAR && (key == VK_RETURN || key == VK_ESCAPE) && !self->m_composing) return 0;
	const auto result = DefSubclassProc(window, message, key, value);
	if (message == WM_SETFOCUS || message == WM_KILLFOCUS)
	{
		InvalidateRect(self->m_panel, nullptr, FALSE);
		if (self->focusChanged) self->focusChanged();
	}
	return result;
}

LRESULT CALLBACK NativeTextInput::WindowProc(HWND window, UINT message, WPARAM key, LPARAM value)
{
	if (message == WM_NCCREATE) SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(value)->lpCreateParams));
	auto* self = reinterpret_cast<NativeTextInput*>(GetWindowLongPtrW(window, GWLP_USERDATA));
	if (!self) return DefWindowProcW(window, message, key, value);
	if (message == WM_ERASEBKGND) return 1;
	if (message == WM_PAINT) { self->Paint(window); return 0; }
	if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORSTATIC)
	{
		SetTextColor(reinterpret_cast<HDC>(key), RGB(245, 245, 245));
		SetBkColor(reinterpret_cast<HDC>(key), RGB(12, 12, 12));
		return reinterpret_cast<LRESULT>(self->m_background);
	}
	if (message == WM_LBUTTONDOWN && window == self->m_panel)
	{
		RECT bounds{};
		GetClientRect(window, &bounds);
		const auto field = InputFieldBounds(bounds.right, bounds.bottom, self->m_dpi);
		POINT point{GET_X_LPARAM(value), GET_Y_LPARAM(value)};
		if (PtInRect(&field, point) && self->Focus())
		{
			// 上下の余白も入力欄として扱い、クリック位置とドラッグ選択を EDIT へ渡す。
			MapWindowPoints(window, self->m_edit, &point, 1);
			RECT client{};
			GetClientRect(self->m_edit, &client);
			point.y = std::clamp<LONG>(point.y, 0, std::max<LONG>(0, client.bottom - 1));
			SendMessageW(self->m_edit, WM_LBUTTONDOWN, key, MAKELPARAM(point.x, point.y));
			return 0;
		}
	}
	if (message == WM_COMMAND)
	{
		if (reinterpret_cast<HWND>(value) == self->m_edit && HIWORD(key) == EN_CHANGE) self->Changed();
		if (HIWORD(key) == BN_CLICKED)
		{
			if (reinterpret_cast<HWND>(value) == self->m_buttons[0]) self->Choose(swkbd::host::Action::Cancel);
			if (reinterpret_cast<HWND>(value) == self->m_buttons[1]) self->Choose(swkbd::host::Action::Accept);
		}
		return 0;
	}
	if (message == WM_DRAWITEM)
	{
		const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(value);
		const auto saved = SaveDC(item->hDC);
		SetDCBrushColor(item->hDC, (item->itemState & ODS_SELECTED) ? RGB(55, 55, 55) : RGB(28, 28, 28));
		FillRect(item->hDC, &item->rcItem, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
		FrameRect(item->hDC, &item->rcItem, static_cast<HBRUSH>(GetStockObject((item->itemState & ODS_FOCUS) ? WHITE_BRUSH : GRAY_BRUSH)));
		SetBkMode(item->hDC, TRANSPARENT);
		SetTextColor(item->hDC, RGB(245, 245, 245));
		SelectObject(item->hDC, self->m_font);
		RECT rect = item->rcItem;
		DrawTextW(item->hDC, item->hwndItem == self->m_buttons[0] ? L"キャンセル" : L"確定", -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		RestoreDC(item->hDC, saved);
		return TRUE;
	}
	if (message == WM_SETFOCUS) { self->Focus(); return 0; }
	if (message == WM_NCDESTROY) SetWindowLongPtrW(window, GWLP_USERDATA, 0);
	return DefWindowProcW(window, message, key, value);
}
