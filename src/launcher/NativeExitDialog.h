#pragma once

#include <Windows.h>
#include <functional>

// UI STA が所有する、ゲーム描画面と同じ root 内の確認表示。
class NativeExitDialog final
{
public:
	explicit NativeExitDialog(HWND parent);
	~NativeExitDialog();
	NativeExitDialog(const NativeExitDialog&) = delete;
	NativeExitDialog& operator=(const NativeExitDialog&) = delete;
	void Show(bool visible, const RECT& gameBounds);
	void Layout(const RECT& gameBounds);
	std::function<void(bool)> result;

private:
	static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM);
	static LRESULT CALLBACK ButtonProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
	void Choose(bool confirm);
	void Paint(HWND hwnd);
	HWND m_parent = nullptr, m_dim = nullptr, m_panel = nullptr, m_buttons[2]{};
	HFONT m_font = nullptr, m_headingFont = nullptr;
	UINT m_dpi = 0;
	ATOM m_class = 0;
	bool m_visible = false, m_pending = false;
};
