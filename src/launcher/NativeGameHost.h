#pragma once

#include <Windows.h>
#include <CommCtrl.h>
#include <functional>

struct ICoreWebView2Controller;

// UI STA が所有する描画専用 HWND。Renderer の停止後に破棄する。
class NativeGameHost final
{
public:
	NativeGameHost(HWND parent, ICoreWebView2Controller* controller, bool diagnostics, bool failAfterCreate = false);
	~NativeGameHost();
	NativeGameHost(const NativeGameHost&) = delete;
	NativeGameHost& operator=(const NativeGameHost&) = delete;

	HWND GetHandle() const { return m_child; }
	HWND GetOpenGLHandle() const { return m_openGLChild; }
	SIZE GetClientSize() const;
	bool IsRenderable() const;
	bool HasLayoutFailed() const { return m_layoutFailed; }
	bool SetBounds(const RECT& bounds);
	void SetVisible(bool visible);
	bool Focus();
	void SetGameLayout(bool enabled);
	bool SetFullscreenLayout(bool enabled);
	bool SetOpenGLComposition(bool enabled);
	void SetControlsVisible(bool visible);
	std::function<void()> stateChanged;
	std::function<void(UINT, WPARAM, LPARAM)> input;
	std::function<void(UINT)> shortcut;

private:
	static LRESULT CALLBACK OpenGLProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK ChildProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK ParentProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data);
	static LRESULT CALLBACK ControlProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data);
	void UpdateLayout();
	void UpdateControls();
	LRESULT ControlMessage(UINT message, WPARAM wParam, LPARAM lParam);
	void Release();
	bool OnUiThread() const;
	void RestoreWebViewFocus();

	HWND m_parent = nullptr;
	HWND m_child = nullptr;
	HWND m_controls[2]{};
	HWND m_tooltip = nullptr;
	bool m_controlsVisible = false;
	HWND m_hoveredControl = nullptr;
	HINSTANCE m_instance = nullptr;
	ICoreWebView2Controller* m_controller = nullptr; // WebView2 より先に破棄する借用参照。
	DWORD m_thread = 0;
	ATOM m_class = 0;
	bool m_subclassed = false;
	bool m_diagnostics = false;
	bool m_visible = false;
	bool m_layoutFailed = false;
	bool m_gameLayout = false;
	bool m_fullscreenLayout = false;
	HWND m_openGLChild = nullptr;
	ATOM m_openGLClass = 0;
};
