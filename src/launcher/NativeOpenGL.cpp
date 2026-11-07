#include "NativeOpenGL.h"

#include "config/CemuConfig.h"
#include <stdexcept>

void LoadOpenGLImports();

namespace
{
	void RequireGl(bool valid, const char* message)
	{
		if (!valid) throw std::runtime_error(message);
	}
} // namespace

NativeOpenGL::NativeOpenGL(HWND window, unsigned width, unsigned height, std::function<void()> presented)
	: m_window(window), m_presented(std::move(presented))
{
	HGLRC bootstrap = nullptr;
	try
	{
		DWORD owner = 0;
		RequireGl(IsWindow(window) && GetWindowThreadProcessId(window, &owner) && owner == GetCurrentProcessId(), "OpenGL: an owned game canvas is required");
		m_dc = GetDC(window);
		RequireGl(m_dc != nullptr, "OpenGL: unable to acquire game canvas DC");
		if (!GetPixelFormat(m_dc))
		{
			PIXELFORMATDESCRIPTOR format{sizeof(PIXELFORMATDESCRIPTOR), 1};
			format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
			format.iPixelType = PFD_TYPE_RGBA; format.cColorBits = 32; format.cAlphaBits = 8;
			format.cDepthBits = 24; format.cStencilBits = 8;
			const auto index = ChoosePixelFormat(m_dc, &format);
			RequireGl(index != 0 && SetPixelFormat(m_dc, index, &format), "OpenGL: pixel format is unavailable");
		}
		bootstrap = wglCreateContext(m_dc);
		RequireGl(bootstrap && wglMakeCurrent(m_dc, bootstrap), "OpenGL: bootstrap context failed");
		const auto create = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(wglGetProcAddress("wglCreateContextAttribsARB"));
		RequireGl(create != nullptr && reinterpret_cast<uintptr_t>(create) > 3 && reinterpret_cast<uintptr_t>(create) != UINTPTR_MAX, "OpenGL: WGL_ARB_create_context is required");
		const int attributes[]{WGL_CONTEXT_MAJOR_VERSION_ARB, 4, WGL_CONTEXT_MINOR_VERSION_ARB, 5,
			WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB, 0};
		m_context = create(m_dc, nullptr, attributes);
		RequireGl(m_context != nullptr, "OpenGL: a 4.5 compatibility context is required");
		RequireGl(wglMakeCurrent(nullptr, nullptr) != FALSE, "OpenGL: bootstrap detach failed");
		wglDeleteContext(bootstrap); bootstrap = nullptr;
		RequireGl(wglMakeCurrent(m_dc, m_context) != FALSE, "OpenGL: context activation failed");
		LoadOpenGLImports();
		RequireGl(glGetString && glGetIntegerv && glCreateTextures && glClipControl && glGenProgramPipelines && glDeleteBuffers,
			"OpenGL: required renderer functions are unavailable");
		GLint limit = 0; glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
		RequireGl(width <= static_cast<unsigned>(limit) && height <= static_cast<unsigned>(limit), "OpenGL: native resolution exceeds texture limits");
		const auto name = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
		RequireGl(name != nullptr, "OpenGL: renderer name is unavailable");
		m_deviceName = name;
		SetOpenGLCanvasCallbacks(this); m_registered = true;
		cemuLog_log(LogType::Force, "NativeHost OpenGL context created: device={} coordinator={} canvas={}", m_deviceName, GetCurrentThreadId(), reinterpret_cast<uintptr_t>(window));
		Detach();
	}
	catch (...)
	{
		wglMakeCurrent(nullptr, nullptr);
		if (bootstrap) wglDeleteContext(bootstrap);
		Release(); throw;
	}
}

NativeOpenGL::~NativeOpenGL() { Release(); }

bool NativeOpenGL::MakeCurrent(bool padView)
{
	return !padView && m_context && wglMakeCurrent(m_dc, m_context);
}

void NativeOpenGL::Detach()
{
	RequireGl(wglMakeCurrent(nullptr, nullptr) != FALSE, "OpenGL: context detach failed");
}

void NativeOpenGL::SwapBuffers(bool swapTV, bool)
{
	if (!swapTV || !MakeCurrent(false)) return;
	if (wglSwapIntervalEXT) wglSwapIntervalEXT(GetConfig().vsync > 0 ? 1 : 0);
	if (::SwapBuffers(m_dc)) m_presented();
}

bool NativeOpenGL::Close() { return Release(); }

bool NativeOpenGL::Release()
{
	// 呼出し元は GPU thread の join を完了してから破棄する。
	if (m_registered) { ClearOpenGLCanvasCallbacks(); m_registered = false; }
	if (m_context)
	{
		wglMakeCurrent(nullptr, nullptr);
		const bool released = wglDeleteContext(m_context) != FALSE;
		cemuLog_log(LogType::Force, "NativeHost OpenGL context released: success={} thread={}", released, GetCurrentThreadId());
		if (!released) return false;
		m_context = nullptr;
	}
	if (m_dc) { if (!ReleaseDC(m_window, m_dc)) return false; m_dc = nullptr; }
	return true;
}
