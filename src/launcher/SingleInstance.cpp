#include "SingleInstance.h"

#include <CommCtrl.h>
#include <sddl.h>
#include <stdexcept>
#include <vector>

namespace
{
	std::wstring UserIdentity()
	{
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("Cannot identify the Windows user");
		DWORD size = 0;
		GetTokenInformation(token, TokenUser, nullptr, 0, &size);
		std::vector<unsigned char> buffer(size);
		const bool loaded = GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != FALSE;
		CloseHandle(token);
		LPWSTR sid = nullptr;
		if (!loaded || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid))
			throw std::runtime_error("Cannot identify the Windows user");
		std::wstring result(sid);
		LocalFree(sid);
		return result;
	}
} // namespace

SingleInstance::SingleInstance()
{
	try
	{
		const auto identity = L"MCWiiU-Launcher." + UserIdentity();
		// 同じユーザーの別セッション、別配置の EXE も同じ保存領域を保護する。
		m_mutex = CreateMutexW(nullptr, FALSE, (L"Global\\" + identity).c_str());
		if (!m_mutex) throw std::runtime_error("Cannot acquire the launcher instance lock");
		const auto wait = WaitForSingleObject(m_mutex, 0);
		m_primary = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
		if (!m_primary && wait != WAIT_TIMEOUT) throw std::runtime_error("Cannot inspect the launcher instance lock");
		m_activation = CreateEventW(nullptr, FALSE, FALSE, (L"Global\\" + identity + L".activate").c_str());
		if (!m_activation) throw std::runtime_error("Cannot create the launcher activation event");
		// PID の共有は同じデスクトップセッションだけでよい。HWND の探索は行わない。
		const auto mappingName = L"Local\\" + identity + L".owner";
		if (m_primary)
		{
			ResetEvent(m_activation);
			m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(DWORD), mappingName.c_str());
			if (!m_mapping) throw std::runtime_error("Cannot publish the launcher owner");
			auto* owner = static_cast<DWORD*>(MapViewOfFile(m_mapping, FILE_MAP_WRITE, 0, 0, sizeof(DWORD)));
			if (!owner) throw std::runtime_error("Cannot publish the launcher owner");
			*owner = GetCurrentProcessId();
			UnmapViewOfFile(owner);
		}
		else
		{
			m_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, mappingName.c_str());
			if (m_mapping)
			{
				const auto* owner = static_cast<const DWORD*>(MapViewOfFile(m_mapping, FILE_MAP_READ, 0, 0, sizeof(DWORD)));
				if (owner) { AllowSetForegroundWindow(*owner); UnmapViewOfFile(owner); }
			}
			SetEvent(m_activation);
		}
	}
	catch (...)
	{
		if (m_mapping) CloseHandle(m_mapping);
		if (m_activation) CloseHandle(m_activation);
		if (m_primary) ReleaseMutex(m_mutex);
		if (m_mutex) CloseHandle(m_mutex);
		throw;
	}
}

SingleInstance::~SingleInstance()
{
	if (m_window && IsWindow(m_window))
	{
		KillTimer(m_window, reinterpret_cast<UINT_PTR>(this));
		RemoveWindowSubclass(m_window, WindowProc, reinterpret_cast<UINT_PTR>(this));
	}
	if (m_mapping) CloseHandle(m_mapping);
	if (m_activation) CloseHandle(m_activation);
	if (m_primary) ReleaseMutex(m_mutex);
	if (m_mutex) CloseHandle(m_mutex);
}

void SingleInstance::Attach(HWND window)
{
	if (!m_primary || !SetWindowSubclass(window, WindowProc, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)))
		throw std::runtime_error("Cannot attach launcher activation");
	m_window = window;
	if (!SetTimer(window, reinterpret_cast<UINT_PTR>(this), 150, nullptr)) throw std::runtime_error("Cannot start launcher activation");
}

LRESULT CALLBACK SingleInstance::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
	auto& instance = *reinterpret_cast<SingleInstance*>(data);
	if (message == WM_TIMER && wParam == id)
	{
		if (WaitForSingleObject(instance.m_activation, 0) == WAIT_OBJECT_0)
		{
			if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
			if (!SetForegroundWindow(window))
			{
				FLASHWINFO flash{sizeof(flash), window, FLASHW_TRAY, 3, 0};
				FlashWindowEx(&flash);
			}
		}
		return 0;
	}
	if (message == WM_NCDESTROY)
	{
		KillTimer(window, id);
		RemoveWindowSubclass(window, WindowProc, id);
		instance.m_window = nullptr;
	}
	return DefSubclassProc(window, message, wParam, lParam);
}
