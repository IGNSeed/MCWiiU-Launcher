#pragma once

#include <Windows.h>
#include <functional>
#include <memory>
#include <string>

// UI への依存を callback に限定した、一回起動・一回終了の Core 所有者。
class CemuHost final
{
public:
	struct Callbacks
	{
		std::function<void(std::function<void()>)> post;
		std::function<bool()> prepareCanvas;
		std::function<void(std::string)> status;
		std::function<void()> stopped;
	};

	CemuHost(HWND root, HWND canvas, Callbacks callbacks);
	~CemuHost();
	CemuHost(const CemuHost&) = delete;
	CemuHost& operator=(const CemuHost&) = delete;
	void Start(bool launchImmediately);
	void LaunchGame();
	void RequestStop();
	void ReportDisplayFailure();
	bool IsStopped() const;
	bool IsRunning() const;
	bool HasFailed() const;
	void UpdateWindowState(SIZE physicalSize, UINT dpi, bool visible, bool focused, bool fullscreen);
	void InputMessage(UINT message, WPARAM wParam, LPARAM lParam);
	std::string StatusJson() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
