#pragma once

#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#include <Windows.h>
#include <functional>

// HWND は UI STA、Context は起動前の coordinator と実行中の GPU thread が順に所有する。
class NativeOpenGL final : public OpenGLCanvasCallbacks
{
public:
	NativeOpenGL(HWND window, unsigned width, unsigned height, std::function<void()> presented);
	~NativeOpenGL();
	NativeOpenGL(const NativeOpenGL&) = delete;
	NativeOpenGL& operator=(const NativeOpenGL&) = delete;
	bool MakeCurrent(bool padView) override;
	void SwapBuffers(bool swapTV, bool swapDRC) override;
	void Detach();
	bool Close();
	std::string DeviceName() const { return m_deviceName; }

private:
	bool Release();
	HWND m_window = nullptr;
	HDC m_dc = nullptr;
	HGLRC m_context = nullptr;
	bool m_registered = false;
	std::string m_deviceName;
	std::function<void()> m_presented;
};
