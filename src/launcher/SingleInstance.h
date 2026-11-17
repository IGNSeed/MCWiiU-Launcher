#pragma once

#include <Windows.h>
#include <string>

class SingleInstance final
{
public:
	SingleInstance();
	~SingleInstance();
	SingleInstance(const SingleInstance&) = delete;
	SingleInstance& operator=(const SingleInstance&) = delete;
	bool IsPrimary() const { return m_primary; }
	void Attach(HWND window);

private:
	HANDLE m_mutex = nullptr, m_activation = nullptr, m_mapping = nullptr;
	bool m_primary = false;
	HWND m_window = nullptr;
	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data);
};
