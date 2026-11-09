#pragma once

#include <Windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

// UI への依存を callback に限定した、Core とタイトルセッションの所有者。
class CemuHost final
{
public:
	struct Callbacks
	{
		std::function<void(std::function<void()>)> post;
		std::function<bool(bool)> prepareCanvas;
		std::function<HWND()> openGLCanvas;
		std::function<void(std::string)> status;
		std::function<bool()> titleStopped;
		std::function<void()> stopped;
	};

	CemuHost(HWND root, HWND canvas, Callbacks callbacks);
	~CemuHost();
	CemuHost(const CemuHost&) = delete;
	CemuHost& operator=(const CemuHost&) = delete;
	void Start(bool launchImmediately);
	void LaunchGame();
	void StopGame();
	bool BeginStopGameConfirmation(uint64_t sessionId);
	void CancelStopGameConfirmation(uint64_t sessionId, uint64_t confirmationId);
	bool ConfirmStopGame(uint64_t sessionId, uint64_t confirmationId);
	bool HasStopGameConfirmation() const;
	void RequestStop();
	void ReportDisplayFailure();
	bool IsStopped() const;
	bool IsRunning() const;
	uint64_t SessionId() const;
	uint64_t StopConfirmationId() const;
	bool IsTitleStopping() const;
	bool HasFailed() const;
	void UpdateWindowState(SIZE physicalSize, UINT dpi, bool visible, bool focused, bool fullscreen);
	void InputMessage(UINT message, WPARAM wParam, LPARAM lParam);
	std::string StatusJson() const;
	void SaveNativeSettings(std::string_view payload);
	void RefreshSettingsInfo();
	std::string InputCommand(uint64_t requestId, std::string_view payload);
	std::string PollInput();
	std::string FriendsCommand(bool refresh);
	std::string ManageFriends(uint64_t requestId, std::string_view payload);
	std::string AvCommand(uint64_t requestId, std::string_view payload);
	void CancelInputCapture();
	std::string ImportCustomNetwork(uint64_t requestId, const std::wstring& selectedFile, bool replaceExisting);
	std::string AccountCommand(uint64_t requestId, std::string_view payload);
	std::string PreviewAccountZip(uint64_t requestId, const std::wstring& selectedFile);
	std::string WorldCommand(uint64_t requestId, std::string_view payload);
	std::string PreviewWorldImage(uint64_t requestId, std::string_view payload, const std::wstring& selectedFile);
	std::string PreviewWorldImport(uint64_t requestId, std::string_view payload, const std::wstring& selected, std::string_view part);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
