#pragma once

#include <Windows.h>
#include <CommCtrl.h>

struct ICoreWebView2Controller;

// UI STA が所有する描画専用 HWND。Core / Vulkan の寿命はまだ接続しない。
class NativeGameHost final
{
public:
	NativeGameHost(HWND parent, ICoreWebView2Controller* controller, bool diagnostics, bool failAfterCreate = false);
	~NativeGameHost();
	NativeGameHost(const NativeGameHost&) = delete;
	NativeGameHost& operator=(const NativeGameHost&) = delete;

	HWND GetHandle() const { return m_child; }
	SIZE GetClientSize() const;
	bool IsRenderable() const;
	bool SetBounds(const RECT& bounds);
	void SetVisible(bool visible);
	bool Focus();

private:
	static LRESULT CALLBACK ChildProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK ParentProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data);
	void UpdateLayout();
	void Release();
	bool OnUiThread() const;
	void RestoreWebViewFocus();

	HWND m_parent = nullptr;
	HWND m_child = nullptr;
	HINSTANCE m_instance = nullptr;
	ICoreWebView2Controller* m_controller = nullptr; // WebView2 より先に破棄する借用参照。
	DWORD m_thread = 0;
	ATOM m_class = 0;
	bool m_subclassed = false;
	bool m_diagnostics = false;
	bool m_visible = false;
	bool m_layoutFailed = false;
};
