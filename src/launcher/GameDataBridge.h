#pragma once

#include "GameInstallationService.h"
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>

// UI スレッドが所有する選択セッション。実パスは WebView2 に公開しない。
class GameDataBridge final
{
public:
	using Service = GameInstallationService;
	struct Callbacks
	{
		std::function<Service::Snapshot()> snapshot;
		std::function<bool()> canManage;
		std::function<Service::Result(Service::Request)> start;
		std::function<void()> cancel;
		std::function<std::future<Service::Analysis>(Service::Path)> analyze;
	};
	using Picker = std::function<std::optional<Service::Path>(bool)>;
	GameDataBridge(Callbacks callbacks, bool nativeAvailable);
	~GameDataBridge();
	std::string Handle(uint64_t requestId, std::string_view payload, const Picker& picker);
	bool IsWorking();
	bool IsCancellable();
	void BeginClose();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
