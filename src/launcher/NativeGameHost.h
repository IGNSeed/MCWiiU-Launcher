#pragma once

#include <Windows.h>
#include <CommCtrl.h>
#include <functional>
#include <memory>
#include <cstdint>
#include "NativeExitDialog.h"
#include "NativeTextInput.h"

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
	HWND GetVulkanHandle() const { return m_vulkanChild; }
	bool BeginPresentation(uint64_t session);
	bool PreparePresentation(uint64_t session, bool openGL);
	bool PresentFirstFrame(uint64_t session);
	void StopPresentation(uint64_t session);
	bool EndPresentation(uint64_t session);
	bool ResetPresentation(uint64_t session);
	void SetStartupMessage(std::wstring message);
	SIZE GetClientSize() const;
	bool IsRenderable() const;
	bool HasLayoutFailed() const { return m_layoutFailed; }
	bool SetBounds(const RECT& bounds);
	void SetVisible(bool visible);
	bool Focus();
	bool HasInputFocus() const;
	NativeTextInput& TextInput() { return *m_textInput; }
	void SetGameLayout(bool enabled);
	bool SetFullscreenLayout(bool enabled);
	bool SetOpenGLComposition(bool enabled);
	void SetControlsVisible(bool visible);
	void SetExitConfirmation(bool visible);
	void SetExitLabels(const std::array<std::wstring, 4>& labels);
	void CancelTouch();
	std::function<void(bool)> exitResult;
	std::function<void()> stateChanged;
	std::function<void(UINT, WPARAM, LPARAM)> input;
	std::function<void(UINT)> shortcut;

private:
	static LRESULT CALLBACK OpenGLProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK CoverProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK ChildProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK ParentProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data);
	static LRESULT CALLBACK ControlProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data);
	bool MouseMessage(HWND hwnd, UINT message, WPARAM key, LPARAM position);
	RECT GameBounds() const;
	void UpdateLayout();
	void UpdateControls();
	void UpdateTextInput();
	bool IsCovered() const;
	LRESULT ControlMessage(UINT message, WPARAM wParam, LPARAM lParam);
	void Release();
	bool OnUiThread() const;
	void RestoreWebViewFocus();

	std::unique_ptr<NativeExitDialog> m_exitDialog;
	std::unique_ptr<NativeTextInput> m_textInput;
	bool m_exitVisible = false;
	HWND m_touchCapture = nullptr;
	uint32_t m_mouseButtons = 0;
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
	HWND m_vulkanChild = nullptr;
	HWND m_vulkanContainer = nullptr;
	HWND m_startupCover = nullptr;
	std::wstring m_startupMessage;
	ATOM m_coverClass = 0, m_vulkanClass = 0;
	enum class Presentation { Hidden, BlackPending, AwaitingFirstFrame, Presenting, Stopping };
	Presentation m_presentation = Presentation::Hidden;
	uint64_t m_presentationSession = 0;
};
