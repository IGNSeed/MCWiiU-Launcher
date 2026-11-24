#pragma once

#include <Windows.h>
#include <CommCtrl.h>
#include <functional>
#include "../Cafe/OS/libs/swkbd/HostInput.h"

// HWND / IME / font は UI STA のみが所有する。
class NativeTextInput final
{
public:
	explicit NativeTextInput(HWND parent);
	~NativeTextInput();
	NativeTextInput(const NativeTextInput&) = delete;
	NativeTextInput& operator=(const NativeTextInput&) = delete;
	void Sync(bool allowed, const RECT& bounds);
	bool HasFocus() const;
	bool Focus();
	std::function<swkbd::host::Snapshot()> read;
	std::function<bool(uint64_t, std::u16string, swkbd::host::Action)> submit;
	std::function<void()> restoreFocus, focusChanged;
	std::function<void(UINT)> shortcut;

private:
	static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM);
	static LRESULT CALLBACK InputProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
	void Create();
	void Layout(const RECT& bounds);
	void Changed();
	void Choose(swkbd::host::Action action);
	std::u16string Text() const;
	void Paint(HWND hwnd);
	HWND m_parent = nullptr, m_dim = nullptr, m_panel = nullptr, m_edit = nullptr, m_buttons[2]{};
	HFONT m_font = nullptr;
	HBRUSH m_background = nullptr;
	ATOM m_class = 0;
	UINT m_dpi = 0;
	uint64_t m_generation = 0, m_revision = 0;
	unsigned m_limit = 0;
	bool m_visible = false, m_composing = false, m_updating = false, m_pending = false, m_imeKey = false;
	RECT m_bounds{};
};
