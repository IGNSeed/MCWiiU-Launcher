#include "CemuHost.h"

#include "Cemu/Startup.h"
#include "Cemu/GameLaunch.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "config/ActiveSettings.h"
#include "input/InputManager.h"
#include "WindowSystem.h"

#include <pugixml.hpp>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <condition_variable>

std::atomic_bool g_isGPUInitFinished{false};

// Native entry は CLI の console 経路を使用しない。
void requireConsole()
{
	throw std::runtime_error("Cemu CLI console operations are unavailable in LauncherShell");
}

namespace
{
	WindowSystem::WindowInfo s_windowInfo{};
	struct HostState;
	std::weak_ptr<HostState> s_currentHost;
	HostState* s_vulkanHost = nullptr;
	PFN_vkQueuePresentKHR s_present = nullptr;
	PFN_vkAcquireNextImageKHR s_acquire = nullptr;
	PFN_vkCreateSwapchainKHR s_createSwapchain = nullptr;
	PFN_vkDestroySwapchainKHR s_destroySwapchain = nullptr;
	PFN_vkDestroySurfaceKHR s_destroySurface = nullptr;

	void Require(bool condition, const char* message)
	{
		if (!condition)
			throw std::runtime_error(message);
	}

	struct UiRequest
	{
		enum class State { Pending, Executing, Cancelled, Completed };
		std::atomic<State> state{State::Pending};
		std::promise<bool> completion;
		std::future<bool> result = completion.get_future();
		bool Start()
		{
			auto expected = State::Pending;
			return state.compare_exchange_strong(expected, State::Executing);
		}
		void Cancel()
		{
			auto expected = State::Pending;
			if (state.compare_exchange_strong(expected, State::Completed))
				completion.set_value(false);
			else
			{
				expected = State::Executing;
				state.compare_exchange_strong(expected, State::Cancelled);
			}
		}
		void Complete(bool executed)
		{
			const auto previous = state.exchange(State::Completed);
			if (previous != State::Completed)
				completion.set_value(executed && previous != State::Cancelled);
		}
	};

	struct HostState : CafeSystem::SystemImplementation, std::enable_shared_from_this<HostState>
	{
		HWND root, canvas;
		DWORD uiThread = GetCurrentThreadId();
		CemuHost::Callbacks callbacks;
		std::mutex uiMutex, statusMutex, workMutex;
		std::condition_variable wake;
		std::vector<std::shared_ptr<UiRequest>> requests;
		bool accepting = true, acceptingTitle = true;
		uint64 generation = 1;
		std::atomic_bool alive{true}, stop{false}, launch{false}, stopped{true}, failed{false}, logReady{false};
		std::atomic_bool titleStop{false};
		std::atomic_uint64_t sessionId{0}, commonInitCount{0}, titleShutdownCount{0};
		std::atomic_uint64_t vulkanInstance{0}, vulkanDevice{0}, vulkanSurface{0}, surfaceCreates{0};
		std::string corePhase = "initializing", titlePhase = "idle", stopReason;
		std::atomic_uint64_t presents{0}, acquires{0}, swapchainCreates{0}, swapchainDestroys{0}, surfaceDestroys{0}, gameListRevision{0};
		std::atomic<double> fps{0};
		std::atomic_uint32_t swapchainWidth{0}, swapchainHeight{0};
		std::string phase = "idle", error;
		fs::path runtime;
		int controllerModifier = -1, controllerExit = -1;

		HostState(HWND rootWindow, HWND gameWindow, CemuHost::Callbacks ui)
			: root(rootWindow), canvas(gameWindow), callbacks(std::move(ui)) {}

		bool Dispatch(std::function<bool()> action, bool wait, bool titleScoped = true)
		{
			if (GetCurrentThreadId() == uiThread)
			{
				uint64 expected;
				{
					std::lock_guard lock(uiMutex);
					if (!accepting || (titleScoped && !acceptingTitle)) return false;
					expected = generation;
				}
				const bool result = action();
				std::lock_guard lock(uiMutex);
				return result && accepting && (!titleScoped || (acceptingTitle && expected == generation));
			}
			auto request = wait ? std::make_shared<UiRequest>() : nullptr;
			{
				std::lock_guard lock(uiMutex);
				if (!accepting || (titleScoped && !acceptingTitle)) return false;
				const auto expected = generation;
				if (request) requests.push_back(request);
				callbacks.post([weak = weak_from_this(), expected, request, titleScoped, action = std::move(action)] {
					auto state = weak.lock();
					bool execute = false;
					if (state)
					{
						std::lock_guard lock(state->uiMutex);
						execute = state->alive && state->accepting && (!titleScoped || (state->acceptingTitle && expected == state->generation)) && (!request || request->Start());
					}
					bool result = false;
					try { if (execute) result = action(); }
					catch (...) { if (state) state->failed = true; }
					if (request)
					{
						request->Complete(result);
						if (state)
						{
							std::lock_guard lock(state->uiMutex);
							std::erase(state->requests, request);
						}
					}
				});
			}
			return request ? request->result.get() : true;
		}

		std::string Json()
		{
			rapidjson::StringBuffer buffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
			writer.StartObject();
			{
				std::lock_guard lock(statusMutex);
				writer.Key("phase"); writer.String(phase.c_str());
				writer.Key("error"); writer.String(error.c_str());
				writer.Key("corePhase"); writer.String(corePhase.c_str());
				writer.Key("titlePhase"); writer.String(titlePhase.c_str());
				writer.Key("stopReason"); writer.String(stopReason.c_str());
				writer.Key("canLaunch"); writer.Bool(phase == "ready" && !launch && !stop && !stopped && !failed);
			}
			writer.Key("sessionId"); writer.Uint64(sessionId);
			writer.Key("commonInitCount"); writer.Uint64(commonInitCount);
			writer.Key("titleShutdownCount"); writer.Uint64(titleShutdownCount);
			writer.Key("titleStopRequested"); writer.Bool(titleStop);
			writer.Key("stopRequested"); writer.Bool(stop);
			writer.Key("stopped"); writer.Bool(stopped);
			writer.Key("failed"); writer.Bool(failed);
			writer.Key("fps"); writer.Double(fps);
			writer.Key("presents"); writer.Uint64(presents);
			writer.Key("acquires"); writer.Uint64(acquires);
			writer.Key("swapchainCreates"); writer.Uint64(swapchainCreates);
			writer.Key("swapchainDestroys"); writer.Uint64(swapchainDestroys);
			writer.Key("surfaceDestroys"); writer.Uint64(surfaceDestroys);
			writer.Key("surfaceCreates"); writer.Uint64(surfaceCreates);
			writer.Key("vulkanInstance"); writer.Uint64(vulkanInstance);
			writer.Key("vulkanDevice"); writer.Uint64(vulkanDevice);
			writer.Key("vulkanSurface"); writer.Uint64(vulkanSurface);
			writer.Key("ppcRunning"); writer.Bool(CafeSystem::IsTitleRunning());
			writer.Key("gpuStopped"); writer.Bool(Latte_GetStopSignal());
			writer.Key("gpuInitialized"); writer.Bool(g_isGPUInitFinished);
			writer.Key("swapchainWidth"); writer.Uint(swapchainWidth);
			writer.Key("swapchainHeight"); writer.Uint(swapchainHeight);
			writer.Key("gameListRevision"); writer.Uint64(gameListRevision);
			writer.Key("width"); writer.Int(s_windowInfo.phys_width);
			writer.Key("height"); writer.Int(s_windowInfo.phys_height);
			writer.Key("focused"); writer.Bool(s_windowInfo.app_active);
			unsigned heldKeys = 0;
			s_windowInfo.iter_keystates([&](const auto& entry) { if (entry.second) ++heldKeys; });
			writer.Key("heldKeys"); writer.Uint(heldKeys);
			{
				std::lock_guard lock(uiMutex);
				writer.Key("generation"); writer.Uint64(generation);
				writer.Key("pendingUiRequests"); writer.Uint64(requests.size());
			}
			writer.EndObject();
			return buffer.GetString();
		}
		void Publish()
		{
			Dispatch([self = shared_from_this()] { self->callbacks.status(self->Json()); return true; }, false, false);
		}
		void Phase(std::string value)
		{
			{
				std::lock_guard lock(statusMutex);
				if (titleStop && (value == "preparing" || value == "vulkan" || value == "running")) return;
				phase = std::move(value);
				if (phase == "preparing") titlePhase = "preparing";
				if (phase == "vulkan") titlePhase = "starting";
				if (phase == "running") titlePhase = "running";
				if (logReady) cemuLog_log(LogType::Force, "NativeHost phase: {}", phase);
			}
			Publish();
		}
		void CancelTitleUi()
		{
			std::vector<std::shared_ptr<UiRequest>> pending;
			{
				std::lock_guard lock(uiMutex);
				acceptingTitle = false;
				++generation;
				pending.swap(requests);
			}
			for (const auto& request : pending) request->Cancel();
			s_windowInfo.set_keystatesup();
			s_windowInfo.app_active = false;
		}
		void StopTitle(std::string reason)
		{
			{
				std::lock_guard workLock(workMutex);
				std::lock_guard statusLock(statusMutex);
				if (stop || !launch || titleStop) return;
				titleStop = true;
				titlePhase = "stopping";
				phase = "title-stopping";
				stopReason = std::move(reason);
				CancelTitleUi();
			}
			Publish();
			wake.notify_all();
		}
		void Stop()
		{
			{
				std::lock_guard workLock(workMutex);
				std::lock_guard statusLock(statusMutex);
				stop = true;
				corePhase = "stopping";
				if (launch) titlePhase = "stopping";
			}
			std::vector<std::shared_ptr<UiRequest>> pending;
			{
				std::lock_guard lock(uiMutex);
				accepting = false;
				++generation;
				pending.swap(requests);
			}
			for (const auto& request : pending) request->Cancel();
			s_windowInfo.set_keystatesup();
			s_windowInfo.app_active = false;
			wake.notify_all();
		}
		void Fail(std::string message)
		{
			failed = true;
			{
				std::lock_guard lock(statusMutex);
				error = std::move(message);
				if (logReady) cemuLog_log(LogType::Force, "NativeHost failure: {}", error);
			}
			Stop();
		}
		void CafeRecreateCanvas() override
		{
			// OSLauncherThread は別スレッドで Prepare 済み。通常停止の成功とは区別する。
			Fail("Guest OS title relaunch requires application shutdown");
		}
		void CafePPCProcessExit() override
		{
			if (logReady) cemuLog_log(LogType::Force, "NativeHost PPC process exited; returnStatus={}", CafeSystem::GetForegroundTitleReturnStatus().value_or(-1));
			if (!CafeSystem::GetForegroundTitleReturnStatus())
				Fail("Guest process exit without a return status");
			else
				StopTitle("guest-exit");
		}
	};

	fs::path ExecutablePath()
	{
		std::wstring path(32768, L'\0');
		const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
		Require(length && length < path.size(), "Unable to locate LauncherShell");
		path.resize(length);
		return path;
	}
	void NoReparsePoints(fs::path path)
	{
		for (; !path.empty(); path = path.parent_path())
		{
			const auto attributes = GetFileAttributesW(path.c_str());
			Require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "Runtime paths must be existing ordinary files and directories");
			if (path == path.root_path()) break;
		}
	}
	fs::path ValidateRuntime()
	{
		fs::path manifest;
		for (auto directory = ExecutablePath().parent_path(); !directory.empty(); directory = directory.parent_path())
		{
			const auto candidate = directory / ".local/runtime/performance/manifest.json";
			if (fs::is_regular_file(candidate)) { manifest = candidate; break; }
			if (directory == directory.root_path()) break;
		}
		Require(!manifest.empty(), "The isolated performance runtime manifest is required");
		NoReparsePoints(manifest);
		std::ifstream stream(manifest);
		std::string text((std::istreambuf_iterator<char>(stream)), {});
		rapidjson::Document document;
		document.Parse(text.c_str());
		Require(!document.HasParseError() && document.IsObject() && document.HasMember("role") && document["role"].IsString() &&
			std::string_view(document["role"].GetString()) == "isolated-performance-working-copy", "Invalid runtime role");
		const auto runtime = manifest.parent_path() / "cemu";
		Require(document.HasMember("runtime") && document["runtime"].IsString() && fs::equivalent(runtime, _utf8ToPath(document["runtime"].GetString())), "Runtime manifest path mismatch");
		NoReparsePoints(runtime / "portable/settings.xml");
		NoReparsePoints(runtime / "portable/mlc01");
		NoReparsePoints(runtime / "games/base/code/Minecraft.Client.rpx");
		pugi::xml_document settings;
		Require(settings.load_file((runtime / "portable/settings.xml").c_str()), "Unable to read runtime settings");
		const auto config = settings.child("content");
		Require(fs::equivalent(_utf8ToPath(config.child_value("mlc_path")), runtime / "portable/mlc01"), "Runtime MLC mismatch");
		Require(std::string_view(config.child("Graphic").child_value("api")) == "1" && !config.child("Account").child("OnlineEnabled").text().as_bool(), "Only offline Vulkan is supported");
		for (const auto& entry : config.child("GamePaths").children())
		{
			const auto gamePath = fs::weakly_canonical(_utf8ToPath(entry.child_value()));
			Require(gamePath == fs::weakly_canonical(runtime / "games"), "Game paths must remain inside the isolated runtime");
		}
		if (auto state = s_currentHost.lock())
		{
			for (const auto& hotkey : config.child("Hotkeys").children())
			{
				int keyboard = 0, controller = -1;
				std::istringstream(hotkey.child_value()) >> keyboard >> controller;
				const std::string_view name(hotkey.name());
				if (name == "modifiers") state->controllerModifier = controller;
				else if (name == "ExitApplication") state->controllerExit = controller;
				else Require(controller < 0, "This host supports only the configured controller exit hotkey");
			}
		}
		for (const auto& [path, expected, version] : std::array<std::tuple<fs::path, std::string_view, unsigned>, 3>{{
			{runtime / "games/base", "00050000101dbe00", 0},
			{runtime / "portable/mlc01/usr/title/0005000e/101dbe00", "0005000e101dbe00", 560},
			{runtime / "portable/mlc01/usr/title/0005000c/101dbe00", "0005000c101dbe00", 496}}})
		{
			NoReparsePoints(path / "code/app.xml");
			pugi::xml_document app;
			Require(app.load_file((path / "code/app.xml").c_str()), "Missing title metadata");
			auto title = std::string(app.child("app").child_value("title_id"));
			std::transform(title.begin(), title.end(), title.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
			Require(title == expected && (!version || std::stoul(app.child("app").child_value("title_version"), nullptr, 16) == version), "The fixed Minecraft v560 and DLC v496 are required");
		}
		return runtime;
	}

	VKAPI_ATTR VkResult VKAPI_CALL Present(VkQueue queue, const VkPresentInfoKHR* info)
	{
		const auto result = s_present(queue, info);
		if (result >= 0 && s_vulkanHost->presents.fetch_add(1) == 0)
			cemuLog_log(LogType::Force, "NativeHost first vkQueuePresentKHR: result={} GPU thread={}", result, GetCurrentThreadId());
		return result;
	}
	VKAPI_ATTR VkResult VKAPI_CALL Acquire(VkDevice device, VkSwapchainKHR chain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* index)
	{
		const auto result = s_acquire(device, chain, timeout, semaphore, fence, index);
		if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) ++s_vulkanHost->acquires;
		return result;
	}
	VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks* allocator, VkSwapchainKHR* chain)
	{
		const auto result = s_createSwapchain(device, info, allocator, chain);
		if (result == VK_SUCCESS)
		{
			++s_vulkanHost->swapchainCreates;
			s_vulkanHost->swapchainWidth = info->imageExtent.width;
			s_vulkanHost->swapchainHeight = info->imageExtent.height;
		}
		cemuLog_log(LogType::Force, "NativeHost swapchain create: {}x{} result={} device=0x{:x} surface=0x{:x}",
			info->imageExtent.width, info->imageExtent.height, result, reinterpret_cast<uintptr_t>(device), reinterpret_cast<uintptr_t>(info->surface));
		return result;
	}
	VKAPI_ATTR void VKAPI_CALL DestroySwapchain(VkDevice device, VkSwapchainKHR chain, const VkAllocationCallbacks* allocator)
	{
		s_destroySwapchain(device, chain, allocator);
		++s_vulkanHost->swapchainDestroys;
		cemuLog_log(LogType::Force, "NativeHost swapchain released on thread {}", GetCurrentThreadId());
	}
	VKAPI_ATTR void VKAPI_CALL DestroySurface(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* allocator)
	{
		s_destroySurface(instance, surface, allocator);
		++s_vulkanHost->surfaceDestroys;
		cemuLog_log(LogType::Force, "NativeHost Vulkan surface released on thread {}", GetCurrentThreadId());
	}
	void InstallVulkanTrace(HostState& state)
	{
		s_vulkanHost = &state;
		s_present = std::exchange(vkQueuePresentKHR, Present);
		s_acquire = std::exchange(vkAcquireNextImageKHR, Acquire);
		s_createSwapchain = std::exchange(vkCreateSwapchainKHR, CreateSwapchain);
		s_destroySwapchain = std::exchange(vkDestroySwapchainKHR, DestroySwapchain);
		s_destroySurface = std::exchange(vkDestroySurfaceKHR, DestroySurface);
	}
	void RemoveVulkanTrace()
	{
		if (!s_vulkanHost) return;
		vkQueuePresentKHR = s_present;
		vkAcquireNextImageKHR = s_acquire;
		vkCreateSwapchainKHR = s_createSwapchain;
		vkDestroySwapchainKHR = s_destroySwapchain;
		vkDestroySurfaceKHR = s_destroySurface;
		s_vulkanHost = nullptr;
	}
} // namespace

struct CemuHost::Impl
{
	std::shared_ptr<HostState> state;
	std::thread worker;

	void ReleaseRenderer()
	{
		// GPU 開始前の取消では LatteThread_Exit が走らない。
		if (g_renderer)
		{
			auto* renderer = g_renderer.get();
			delete renderer;
			g_renderer.release();
		}
		RemoveVulkanTrace();
		state->vulkanInstance = state->vulkanDevice = state->vulkanSurface = 0;
		g_isGPUInitFinished = false;
	}
	void Run()
	{
		const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		bool commonStarted = false;
		try
		{
			Require(SUCCEEDED(apartment), "Core coordinator requires MTA");
			state->Phase("validating");
			state->runtime = ValidateRuntime();
			const auto portable = state->runtime / "portable";
			std::set<fs::path> failedPaths;
			ActiveSettings::SetPaths(true, ExecutablePath(), portable, portable, portable, state->runtime, failedPaths);
			Require(failedPaths.empty(), "Isolated runtime is not writable");
			GetConfigHandle().SetFilename((portable / "settings.xml").wstring());
			GetConfigHandle().Load();
			NetworkConfig::LoadOnce();
			ActiveSettings::Init();
			state->logReady = true;
			cemuLog_log(LogType::Force, "NativeHost UI STA={} coordinator MTA={} root={} canvas={}", state->uiThread, GetCurrentThreadId(), reinterpret_cast<uintptr_t>(state->root), reinterpret_cast<uintptr_t>(state->canvas));
			state->Phase("initializing");
			LatteOverlay_init();
			CafeSystem::SetImplementation(state.get());
			commonStarted = true;
			++state->commonInitCount;
			CemuCommonInit(state->runtime);
			Require(fs::equivalent(ActiveSettings::GetMlcPath(), portable / "mlc01"), "Loaded MLC escaped the isolated runtime");
			Require(InitializeGlobalVulkan(), "Vulkan loader is unavailable");
			{ std::lock_guard lock(state->statusMutex); state->corePhase = "ready"; }
			state->Phase("ready");
			while (!state->stop)
			{
				{
					std::unique_lock lock(state->workMutex);
					state->wake.wait(lock, [&] { return state->stop || state->launch; });
				}
				if (!state->stop)
				{
					if (!state->sessionId) ++state->sessionId;
					if (!state->titleStop) state->Phase("preparing");
					if (!state->Dispatch([state = state] { return state->callbacks.prepareCanvas(); }, true))
					{
						Require(state->stop || state->titleStop, "Canvas preparation failed");
					}
					if (!state->stop && !state->titleStop)
					{
						auto prepared = GameLaunch::Prepare(state->runtime / "games/base");
						Require(prepared.error == GameLaunch::Error::None && CafeSystem::GetForegroundTitleId() == 0x00050000101dbe00ULL && CafeSystem::GetForegroundTitleVersion() == 560, "Minecraft v560 preparation failed");
						if (!state->stop && !state->titleStop)
						{
							state->Phase("vulkan");
							const Vector2i size{s_windowInfo.phys_width, s_windowInfo.phys_height};
							Require(size.x > 0 && size.y > 0, "Game canvas has no drawable size");
							g_renderer = std::make_unique<VulkanRenderer>();
							const auto renderer = VulkanRenderer::GetInstance();
							renderer->InitializeSurface(size, true);
							const auto& chain = renderer->GetChainInfo(true);
							const auto families = VulkanRenderer::FindQueueFamilies(chain.m_surface, renderer->GetPhysicalDevice());
							Require(families.IsComplete() && chain.m_swapchain && chain.m_surface, "Native Vulkan swapchain is incomplete");
							cemuLog_log(LogType::Force, "NativeHost Vulkan instance={} physicalDevice={} surface={} swapchain={} extent={}x{} graphicsFamily={} presentFamily={} device=0x{:x}",
								reinterpret_cast<uintptr_t>(renderer->GetVkInstance()), reinterpret_cast<uintptr_t>(renderer->GetPhysicalDevice()), reinterpret_cast<uintptr_t>(chain.m_surface), reinterpret_cast<uintptr_t>(chain.m_swapchain), chain.getExtent().width, chain.getExtent().height, families.graphicsFamily, families.presentFamily, reinterpret_cast<uintptr_t>(renderer->GetLogicalDevice()));
							state->vulkanInstance = reinterpret_cast<uintptr_t>(renderer->GetVkInstance());
							state->vulkanDevice = reinterpret_cast<uintptr_t>(renderer->GetLogicalDevice());
							state->vulkanSurface = reinterpret_cast<uintptr_t>(chain.m_surface);
							++state->surfaceCreates;
							++state->swapchainCreates;
							state->swapchainWidth = chain.getExtent().width;
							state->swapchainHeight = chain.getExtent().height;
							InstallVulkanTrace(*state);
							g_isGPUInitFinished = false;
							if (!state->stop && !state->titleStop) CafeSystem::LaunchForegroundTitle();
							state->Phase("running");
							std::unique_lock lock(state->workMutex);
							state->wake.wait(lock, [&] { return state->stop || state->titleStop; });
						}
					}
					// coordinator だけが停止・join・Renderer 解放を行う。UI STA は待たない。
					CafeSystem::ShutdownTitle();
					Require(!CafeSystem::IsTitleRunning() && Latte_GetStopSignal(), "Title threads did not stop");
					ReleaseRenderer();
					Require(state->swapchainCreates == state->swapchainDestroys && state->surfaceCreates == state->surfaceDestroys, "Title Vulkan resources were not released");
					++state->titleShutdownCount;
					state->fps = 0;
					state->swapchainWidth = state->swapchainHeight = 0;
					cemuLog_log(LogType::Force, "NativeHost title shutdown complete: session={} rendererPresent={} GPUStopped={}", state->sessionId.load(), bool(g_renderer), Latte_GetStopSignal());
					if (!state->stop)
					{
						state->Phase("title-stopped");
						Require(state->Dispatch([state = state] { return state->callbacks.titleStopped(); }, true, false) || state->stop, "Unable to restore Launcher Home");
						{
							std::lock_guard workLock(state->workMutex);
							std::lock_guard statusLock(state->statusMutex);
							state->launch = false;
							state->titlePhase = "stopped";
							if (!state->stop) state->phase = "ready";
						}
						state->Publish();
					}
				}
			}
		} catch (const std::exception& error)
		{
			state->Fail(error.what());
		}
		state->Stop();
		if (commonStarted)
		{
			state->Phase("stopping");
			CafeSystem::Shutdown();
			cemuLog_log(LogType::Force, "NativeHost CafeSystem shutdown complete; rendererPresent={}", bool(g_renderer));
			// GPU 未開始の取消でも、既存 LatteThread_Exit と同じく destructor 中は
			// g_renderer を有効に保つ。内部資源の解放がこの参照を利用する。
			ReleaseRenderer();
			InputManager::instance().Shutdown();
			cemuLog_log(LogType::Force, "NativeHost input shutdown complete; presents={} acquires={} swapchainsReleased={} surfacesReleased={}",
				state->presents.load(), state->acquires.load(), state->swapchainDestroys.load(), state->surfaceDestroys.load());
			CafeSystem::SetImplementation(nullptr);
			g_isGPUInitFinished = false;
			cemuLog_waitForFlush();
		}
		if (SUCCEEDED(apartment)) CoUninitialize();
		state->stopped = true;
		{
			std::lock_guard lock(state->statusMutex);
			state->phase = state->failed ? "failed" : "stopped";
			state->corePhase = state->phase;
			state->titlePhase = state->failed ? "failed" : state->sessionId ? "stopped" : "idle";
		}
		if (state->logReady)
		{
			cemuLog_log(LogType::Force, "NativeHost final state: {}", state->Json());
			cemuLog_waitForFlush();
		}
		state->callbacks.post([weak = state->weak_from_this()] {
			if (auto current = weak.lock(); current && current->alive)
			{
				current->callbacks.status(current->Json());
				current->callbacks.stopped();
			}
		});
	}
};

CemuHost::CemuHost(HWND root, HWND canvas, Callbacks callbacks) : m_impl(std::make_unique<Impl>())
{
	Require(s_currentHost.expired(), "Only one Core host may exist");
	m_impl->state = std::make_shared<HostState>(root, canvas, std::move(callbacks));
	s_currentHost = m_impl->state;
	s_windowInfo.window_main = {WindowSystem::WindowHandleInfo::Backend::Windows, nullptr, canvas};
	s_windowInfo.canvas_main = s_windowInfo.window_main;
	s_windowInfo.pad_open = false;
	s_windowInfo.pad_dpi_scale = 1;
}
CemuHost::~CemuHost()
{
	if (!IsStopped()) std::terminate();
	m_impl->state->alive = false;
	if (m_impl->worker.joinable()) m_impl->worker.join();
	s_currentHost.reset();
}
void CemuHost::Start(bool launchImmediately)
{
	Require(!m_impl->worker.joinable(), "Core initialization may run only once");
	m_impl->state->launch = launchImmediately;
	m_impl->state->stopped = false;
	try { m_impl->worker = std::thread([impl = m_impl.get()] { impl->Run(); }); }
	catch (...) { m_impl->state->stopped = true; throw; }
}
void CemuHost::LaunchGame()
{
	{
		std::lock_guard workLock(m_impl->state->workMutex);
		std::lock_guard statusLock(m_impl->state->statusMutex);
		if (m_impl->state->phase != "ready" || m_impl->state->stop || m_impl->state->launch || m_impl->state->failed) return;
		m_impl->state->launch = true;
		m_impl->state->titleStop = false;
		++m_impl->state->sessionId;
		m_impl->state->titlePhase = "preparing";
		m_impl->state->stopReason.clear();
		{ std::lock_guard lock(m_impl->state->uiMutex); m_impl->state->acceptingTitle = true; ++m_impl->state->generation; }
		m_impl->state->phase = "preparing";
	}
	m_impl->state->Publish();
	m_impl->state->wake.notify_all();
}
void CemuHost::StopGame() { m_impl->state->StopTitle("emulator-stop"); }
void CemuHost::RequestStop() { m_impl->state->Stop(); }
void CemuHost::ReportDisplayFailure() { m_impl->state->Fail("Unable to switch launcher display mode"); }
bool CemuHost::IsStopped() const { return m_impl->state->stopped; }
bool CemuHost::IsRunning() const
{
	std::lock_guard lock(m_impl->state->statusMutex);
	return m_impl->state->phase == "running" && !m_impl->state->stop;
}
bool CemuHost::HasFailed() const { return m_impl->state->failed; }
std::string CemuHost::StatusJson() const { return m_impl->state->Json(); }
void CemuHost::UpdateWindowState(SIZE size, UINT dpi, bool visible, bool focused, bool fullscreen)
{
	const double scale = static_cast<double>(dpi) / 96;
	if (size.cx > 0 && size.cy > 0)
	{
		s_windowInfo.phys_width = size.cx;
		s_windowInfo.phys_height = size.cy;
		s_windowInfo.width = static_cast<int>(size.cx / scale);
		s_windowInfo.height = static_cast<int>(size.cy / scale);
	}
	s_windowInfo.dpi_scale = scale;
	s_windowInfo.is_fullscreen = fullscreen;
	s_windowInfo.app_active = visible && focused && IsRunning() && !m_impl->state->titleStop;
	if (!s_windowInfo.app_active) s_windowInfo.set_keystatesup();
}
void CemuHost::InputMessage(UINT message, WPARAM key, LPARAM value)
{
	if (message == WM_KILLFOCUS || message == WM_CANCELMODE) { s_windowInfo.set_keystatesup(); s_windowInfo.app_active = false; return; }
	if (message == WM_SETFOCUS) { s_windowInfo.app_active = IsRunning() && !m_impl->state->titleStop; return; }
	if ((message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) && s_windowInfo.app_active)
	{
		UINT code = static_cast<UINT>(key);
		if (code == VK_SHIFT) code = MapVirtualKeyW((static_cast<UINT>(value) >> 16) & 255, MAPVK_VSC_TO_VK_EX);
		if (code == VK_CONTROL) code = (value & (1LL << 24)) ? VK_RCONTROL : VK_LCONTROL;
		if (code == VK_MENU) code = (value & (1LL << 24)) ? VK_RMENU : VK_LMENU;
		s_windowInfo.set_keystate(code, message == WM_KEYDOWN || message == WM_SYSKEYDOWN);
	}
}

namespace WindowSystem
{
	WindowInfo& GetWindowInfo() { return s_windowInfo; }
	void Create() { throw std::logic_error("Native WindowSystem is owned by LauncherShell"); }
	void GetWindowSize(int& w, int& h) { w = s_windowInfo.width; h = s_windowInfo.height; }
	void GetWindowPhysSize(int& w, int& h) { w = s_windowInfo.phys_width; h = s_windowInfo.phys_height; }
	void GetPadWindowSize(int& w, int& h) { w = s_windowInfo.pad_width; h = s_windowInfo.pad_height; }
	void GetPadWindowPhysSize(int& w, int& h) { w = s_windowInfo.phys_pad_width; h = s_windowInfo.phys_pad_height; }
	double GetWindowDPIScale() { return s_windowInfo.dpi_scale; }
	double GetPadDPIScale() { return s_windowInfo.pad_dpi_scale; }
	bool IsPadWindowOpen() { return s_windowInfo.pad_open; }
	bool IsFullScreen() { return s_windowInfo.is_fullscreen; }
	bool InputConfigWindowHasFocus()
	{
		// ゲーム以外が入力を所有する間は、既存の VPAD/KPAD 抑止経路を使う。
		return !s_windowInfo.app_active;
	}
	bool IsKeyDown(uint32 key) { return s_windowInfo.get_keystate(key); }
	bool IsKeyDown(PlatformKeyCodes key)
	{
		switch (key)
		{
		case PlatformKeyCodes::LCONTROL: return IsKeyDown(VK_LCONTROL);
		case PlatformKeyCodes::RCONTROL: return IsKeyDown(VK_RCONTROL);
		case PlatformKeyCodes::TAB: return IsKeyDown(VK_TAB);
		case PlatformKeyCodes::ESCAPE: return IsKeyDown(VK_ESCAPE);
		}
		return false;
	}
	std::string GetKeyCodeName(uint32 key)
	{
		const auto scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX);
		wchar_t name[128]{};
		const LONG flags = static_cast<LONG>((scan & 255) << 16) | (HIBYTE(scan) ? 1L << 24 : 0);
		return GetKeyNameTextW(flags, name, 128) ? boost::nowide::narrow(name) : fmt::format("key_{}", key);
	}
	bool NotifyGameLoaded()
	{
		auto state = s_currentHost.lock();
		if (!state) return false;
		const bool ready = state->Dispatch([state] { return g_renderer && state->callbacks.prepareCanvas(); }, true);
		cemuLog_log(LogType::Force, "NativeHost NotifyGameLoaded completed={}", ready);
		return ready;
	}
	void NotifyGameExited() { if (auto state = s_currentHost.lock()) state->StopTitle("game-exited-notification"); }
	void RefreshGameList()
	{
		if (auto state = s_currentHost.lock()) { ++state->gameListRevision; state->Publish(); }
	}
	void UpdateWindowTitles(bool idle, bool loading, double fps)
	{
		if (auto state = s_currentHost.lock())
		{
			if (state->titleStop || state->stop) return;
			state->fps = fps;
			state->Dispatch([state, idle, loading, fps] {
				const auto title = idle ? L"MCWiiU Launcher" : loading ? L"MCWiiU Launcher - Loading" :
					boost::nowide::widen(fmt::format("MCWiiU Launcher - FPS: {:.2f} [Vulkan] [TitleId: 00050000-101dbe00] [JP v560]", fps));
				SetWindowTextW(state->root, title.c_str());
				state->callbacks.status(state->Json());
				return true;
			}, false);
		}
	}
	void ShowErrorDialog(std::string_view message, std::string_view title, std::optional<ErrorCategory>)
	{
		if (auto state = s_currentHost.lock()) state->Fail(fmt::format("{}: {}", title, message));
	}
	void CaptureInput(const ControllerState& current, const ControllerState& last)
	{
		if (auto state = s_currentHost.lock(); state && s_windowInfo.app_active && state->controllerModifier >= 0 && state->controllerExit >= 0 &&
			current.buttons.GetButtonState(state->controllerModifier) && current.buttons.GetButtonState(state->controllerExit) && !last.buttons.GetButtonState(state->controllerExit))
			state->Stop();
	}
} // namespace WindowSystem
