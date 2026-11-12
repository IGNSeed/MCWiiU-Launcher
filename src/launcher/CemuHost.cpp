#include "CemuHost.h"
#include "NativeSettings.h"
#include "InputService.h"
#include "AvSettings.h"
#include "AccountService.h"
#include "FriendsService.h"
#include "WorldService.h"
#include "TextureService.h"
#include "Cafe/Account/Account.h"
#include "Cafe/TitleList/SaveInfo.h"
#include "Cemu/nex/FriendsSession.h"
#include "NativeOpenGL.h"

#include "Cemu/Startup.h"
#include "Cemu/GameLaunch.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/IOSU/legacy/iosu_act.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Core/LatteTiming.h"
#include "Cafe/HW/Latte/Core/MinecraftNativeResolution.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "config/ActiveSettings.h"
#include "input/InputManager.h"
#include <windowsx.h>
#include "input/api/Controller.h"
#include "audio/IAudioAPI.h"
#include "WindowSystem.h"

#include <pugixml.hpp>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <condition_variable>
#include <chrono>
#include <openssl/sha.h>

std::atomic_bool g_isGPUInitFinished{false};

// Native entry は CLI の console 経路を使用しない。
void requireConsole()
{
	throw std::runtime_error("Cemu CLI console operations are unavailable in LauncherShell");
}

namespace
{
	WindowSystem::WindowInfo s_windowInfo{};
	std::atomic_bool s_inputRecoveryPending{false};
	struct HostState;
	std::weak_ptr<HostState> s_currentHost;
	HostState* s_vulkanHost = nullptr;
	PFN_vkQueuePresentKHR s_present = nullptr;
	PFN_vkAcquireNextImageKHR s_acquire = nullptr;
	PFN_vkCreateSwapchainKHR s_createSwapchain = nullptr;
	PFN_vkDestroySwapchainKHR s_destroySwapchain = nullptr;
	PFN_vkDestroySurfaceKHR s_destroySurface = nullptr;
	PFN_vkCreateImage s_createImage = nullptr;

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
		uint64 confirmationCounter = 0, confirmationId = 0, confirmationSession = 0, confirmationGeneration = 0;
		std::atomic_bool alive{true}, stop{false}, launch{false}, stopped{true}, failed{false}, logReady{false};
		std::atomic_bool titleStop{false};
		std::atomic_bool inputReady{false}, networkTest{false};
		std::atomic_uint64_t sessionId{0}, commonInitCount{0}, titleShutdownCount{0};
		std::atomic_uint64_t vulkanInstance{0}, vulkanDevice{0}, vulkanSurface{0}, surfaceCreates{0};
		std::atomic_uint64_t glContextsCreated{0}, glContextsDestroyed{0}, glPresents{0};
		std::string activeRenderer, rendererDevice;
		std::string corePhase = "initializing", titlePhase = "idle", stopReason;
		std::atomic_uint64_t inputReconnectCount{0};
		std::atomic_uint64_t presents{0}, acquires{0}, swapchainCreates{0}, swapchainDestroys{0}, surfaceDestroys{0}, gameListRevision{0};
		std::atomic<double> fps{0};
		std::atomic_uint32_t swapchainWidth{0}, swapchainHeight{0};
		std::string phase = "idle", error;
		fs::path runtime;
		NativeSettings nativeSettings;
		std::unique_ptr<InputService> input;
		std::unique_ptr<AvSettings> av;
		std::unique_ptr<AccountService> accounts;
		std::unique_ptr<WorldService> worlds;
		std::unique_ptr<TextureService> textures;
		bool textureBusy = false;
		uint64_t lastTextureRequest = 0;
		std::chrono::steady_clock::time_point nextTextureFolderOpen{};
		uint64_t lastWorldRequest = 0;
		bool openingWorldFolder = false;
		std::chrono::steady_clock::time_point nextWorldFolderOpen{};
		uint64_t lastAccountRequest = 0;
		bool friendsStarted = false;
		uint64_t lastAvRequest = 0;
		uint64_t lastInputRequest = 0;
		std::string settingsNotice = "loading", settingsInfo = "{}";
		std::mutex imageMutex;
		std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, uint64_t> images;
		std::atomic_uint32_t appliedVsync{0};
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
					catch (const std::exception& error)
					{
						if (state) { cemuLog_log(LogType::Force, "NativeHost UI operation failed: {}", error.what()); state->failed = true; }
					}
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

		void RefreshInputConnection()
		{
			std::lock_guard workLock(workMutex);
			std::lock_guard statusLock(statusMutex);
			if (phase != "running" || !CanEditInputLocked()) return;
			const bool neutral = input->RefreshConnection([] {
				s_inputRecoveryPending = true;
				s_windowInfo.set_keystatesup();
			});
			if (neutral && s_inputRecoveryPending.exchange(false))
			{
				++inputReconnectCount;
				cemuLog_log(LogType::Force, "NativeHost controller reconnected without Settings: session={}", sessionId.load());
			}
		}

		bool CanStopGameLocked() const
		{
			return corePhase == "ready" && phase == "running" && titlePhase == "running" && launch && !stop && !titleStop && !stopped && !failed;
		}
		bool CanEditSettingsLocked() const
		{
			return CanEditWorldsLocked() && (!worlds || worlds->IsHealthy()) && (!textures || textures->IsHealthy());
		}
		bool CanEditWorldsLocked() const
		{
			return !textureBusy && !openingWorldFolder && corePhase == "ready" && phase == "ready" && !launch && !stop && !titleStop && !stopped && !failed && !confirmationId && (!input || input->IsHealthy()) && (!av || av->IsHealthy()) && (!accounts || (accounts->IsHealthy() && !accounts->IsTesting()));
		}
		WorldService::Context WorldContext() const
		{
			WorldService::Context result{ActiveSettings::GetPersistentId(), accounts->Revision(), CanEditWorldsLocked(), {}};
			for (const auto& account : Account::GetAccounts())
				result.accounts.push_back({account.GetPersistentId(), boost::nowide::narrow(account.GetMiiName().data(), account.GetMiiName().size())});
			return result;
		}
		bool CanEditLiveSettingsLocked() const
		{
			return corePhase == "ready" && (phase == "ready" || phase == "running") && !stop && !titleStop && !stopped && !failed && !confirmationId;
		}
		bool CanEditInputLocked() const { return CanEditLiveSettingsLocked() && input && input->IsHealthy(); }
		bool CanEditAudioLocked() const { return CanEditLiveSettingsLocked() && av && av->IsHealthy(); }
		void ClearStopConfirmationLocked()
		{
			confirmationId = confirmationSession = confirmationGeneration = 0;
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
				writer.Key("canLaunch"); writer.Bool(CanEditSettingsLocked());
				writer.Key("canStopGame"); writer.Bool(CanStopGameLocked());
				writer.Key("stopConfirmationId"); writer.Uint64(confirmationId);
				writer.Key("stopConfirmationSessionId"); writer.Uint64(confirmationSession);
				writer.Key("canEditAccounts"); writer.Bool(CanEditSettingsLocked() && accounts != nullptr);
				writer.Key("canEditWorlds"); writer.Bool(CanEditSettingsLocked() && worlds != nullptr);
				writer.Key("canRecoverWorlds"); writer.Bool(CanEditWorldsLocked() && worlds != nullptr);
				writer.Key("canEditTextures"); writer.Bool(CanEditWorldsLocked() && textures && (!worlds || worlds->IsHealthy()));
				writer.Key("textureBusy"); writer.Bool(textureBusy);
				writer.Key("nativeExitConfirmation"); writer.Bool(true);
				writer.Key("canEditNativeSettings"); writer.Bool(CanEditSettingsLocked());
				writer.Key("inputRecoveryPending"); writer.Bool(s_inputRecoveryPending);
				writer.Key("inputReconnectCount"); writer.Uint64(inputReconnectCount);
				writer.Key("canEditInput"); writer.Bool(CanEditInputLocked());
				writer.Key("canEditAudio"); writer.Bool(CanEditAudioLocked());
				writer.Key("canEditOverlay"); writer.Bool(CanEditAudioLocked());
				writer.Key("nativeSettings");
				const auto settings = nativeSettings.Json();
				writer.RawValue(settings.data(), settings.size(), rapidjson::kObjectType);
				writer.Key("activeRenderer"); writer.String(activeRenderer.c_str());
				writer.Key("rendererDevice"); writer.String(rendererDevice.c_str());
				writer.Key("settingsNotice"); writer.String(settingsNotice.c_str());
				writer.Key("settingsInfo"); writer.RawValue(settingsInfo.data(), settingsInfo.size(), rapidjson::kObjectType);
			}
			writer.Key("glContextsCreated"); writer.Uint64(glContextsCreated);
			writer.Key("glContextsDestroyed"); writer.Uint64(glContextsDestroyed);
			writer.Key("glPresents"); writer.Uint64(glPresents);
			writer.Key("appliedNativeVsync"); writer.Uint(appliedVsync);
			writer.Key("nativeResolutionActive"); writer.Bool(MinecraftNativeResolution::IsActive());
			writer.Key("images"); writer.StartArray();
			{
				std::lock_guard lock(imageMutex);
				for (const auto& [key, count] : images)
				{
					const auto [width, height, format, usage] = key;
					writer.StartObject();
					writer.Key("width"); writer.Uint(width); writer.Key("height"); writer.Uint(height);
					writer.Key("format"); writer.Uint(format); writer.Key("usage"); writer.Uint(usage);
					writer.Key("count"); writer.Uint64(count); writer.EndObject();
				}
			}
			writer.EndArray();
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
		bool StopTitle(std::string reason, uint64 expectedSession = 0, uint64 expectedConfirmation = 0)
		{
			{
				std::lock_guard workLock(workMutex);
				std::lock_guard statusLock(statusMutex);
				if (stop || !launch || titleStop) return false;
				if (expectedSession)
				{
					std::lock_guard uiLock(uiMutex);
					if (!CanStopGameLocked() || sessionId != expectedSession || !expectedConfirmation ||
						confirmationId != expectedConfirmation || confirmationSession != expectedSession || confirmationGeneration != generation)
						return false;
				}
				ClearStopConfirmationLocked();
				titleStop = true;
				titlePhase = "stopping";
				phase = "title-stopping";
				stopReason = std::move(reason);
				CancelTitleUi();
			}
			Publish();
			wake.notify_all();
			return true;
		}
		void Stop()
		{
			{
				std::lock_guard workLock(workMutex);
				std::lock_guard statusLock(statusMutex);
				stop = true;
				ClearStopConfirmationLocked();
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
		const std::string_view api = config.child("Graphic").child_value("api");
		Require(api == "0" || api == "1", "Only Vulkan or OpenGL is supported");
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
	VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice device, const VkImageCreateInfo* info, const VkAllocationCallbacks* allocator, VkImage* image)
	{
		const auto result = s_createImage(device, info, allocator, image);
		if (result == VK_SUCCESS)
		{
			std::lock_guard lock(s_vulkanHost->imageMutex);
			const auto key = std::tuple{info->extent.width, info->extent.height, uint32_t(info->format), uint32_t(info->usage)};
			if (s_vulkanHost->images.contains(key) || s_vulkanHost->images.size() < 64)
				++s_vulkanHost->images[key];
		}
		return result;
	}
	void InstallVulkanTrace(HostState& state)
	{
		s_vulkanHost = &state;
		s_present = std::exchange(vkQueuePresentKHR, Present);
		s_acquire = std::exchange(vkAcquireNextImageKHR, Acquire);
		s_createSwapchain = std::exchange(vkCreateSwapchainKHR, CreateSwapchain);
		s_destroySwapchain = std::exchange(vkDestroySwapchainKHR, DestroySwapchain);
		s_destroySurface = std::exchange(vkDestroySurfaceKHR, DestroySurface);
		s_createImage = std::exchange(vkCreateImage, CreateImage);
	}
	void RemoveVulkanTrace()
	{
		if (!s_vulkanHost) return;
		vkQueuePresentKHR = s_present;
		vkAcquireNextImageKHR = s_acquire;
		vkCreateSwapchainKHR = s_createSwapchain;
		vkDestroySwapchainKHR = s_destroySwapchain;
		vkDestroySurfaceKHR = s_destroySurface;
		vkCreateImage = s_createImage;
		s_vulkanHost = nullptr;
	}
} // namespace

struct CemuHost::Impl
{
	std::shared_ptr<HostState> state;
	std::thread worker;
	std::thread textureWorker;
	std::unique_ptr<NativeOpenGL> openGL;

	void ReleaseRenderer()
	{
		// GPU 開始前の取消では LatteThread_Exit が走らない。
		if (g_renderer)
		{
			if (openGL) Require(openGL->MakeCurrent(false), "Unable to activate OpenGL for renderer cleanup");
			auto* renderer = g_renderer.get();
			delete renderer;
			g_renderer.release();
		}
		if (openGL) { Require(openGL->Close(), "OpenGL resources were not released"); openGL.reset(); ++state->glContextsDestroyed; }
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
			// 中断した取引がある場合は、Default Account 作成を含む共通初期化より前に止める。
			Require(!fs::exists(portable / ".launcher-account-transaction"), "Account recovery is required before starting Core");
			const auto nativePath = portable / "launcher-native-settings.json";
			if (fs::exists(nativePath)) NoReparsePoints(nativePath);
			{
				std::lock_guard lock(state->statusMutex);
				state->nativeSettings = NativeSettings::Load(nativePath, state->settingsNotice);
			}
			std::set<fs::path> failedPaths;
			ActiveSettings::SetPaths(true, ExecutablePath(), portable, portable, portable, state->runtime, failedPaths);
			Require(failedPaths.empty(), "Isolated runtime is not writable");
			GetConfigHandle().SetFilename((portable / "settings.xml").wstring());
			GetConfigHandle().Load();
			AccountService::EnforceOffline();
			NetworkConfig::LoadOnce();
			ActiveSettings::Init();
			state->logReady = true;
			cemuLog_log(LogType::Force, "NativeHost UI STA={} coordinator MTA={} root={} canvas={}", state->uiThread, GetCurrentThreadId(), reinterpret_cast<uintptr_t>(state->root), reinterpret_cast<uintptr_t>(state->canvas));
			state->Phase("initializing");
			LatteOverlay_init();
			CafeSystem::SetImplementation(state.get());
			commonStarted = true;
			++state->commonInitCount;
			CemuCommonInit(state->runtime, false);
			state->inputReady = true;
			cemuLog_log(LogType::Force, "NativeHost graphic packs: disabled; rules files are not loaded");
			Require(fs::equivalent(ActiveSettings::GetMlcPath(), portable / "mlc01"), "Loaded MLC escaped the isolated runtime");
			InitializeGlobalVulkan();
			auto textures = std::make_unique<TextureService>(portable, state->runtime / "games/base/content/Common", portable / "mlc01/usr/title/0005000e/101dbe00/content/Common");
			{
				std::lock_guard lock(state->statusMutex);
				state->input = std::make_unique<InputService>(); state->av = std::make_unique<AvSettings>();
				state->accounts = std::make_unique<AccountService>();
				state->worlds = std::make_unique<WorldService>(ActiveSettings::GetConfigPath(), SaveInfo(0x00050000101dbe00ULL).GetPath());
				state->textures = std::move(textures);
				state->corePhase = "ready";
			}
			state->Phase("ready");
			while (!state->stop)
			{
				{
					std::unique_lock lock(state->workMutex);
					state->wake.wait(lock, [&] { return state->stop || state->launch || state->networkTest; });
				}
				if (state->networkTest.exchange(false) && !state->stop)
				{
					state->accounts->RunConnectionTest();
					state->Publish();
					continue;
				}
				if (!state->stop)
				{
					if (!state->sessionId) ++state->sessionId;
					if (!state->titleStop) state->Phase("preparing");
					try
					{
						if (!state->Dispatch([state = state] { return state->callbacks.prepareCanvas(GetConfig().graphic_api == kOpenGL); }, true))
						{
							Require(state->stop || state->titleStop, "Canvas preparation failed");
						}
						if (!state->stop && !state->titleStop)
						{
							state->accounts->PrepareLaunch();
							// Coreは同一プロセスに残るため、ACTだけ次の初期化で選択Accountを再読込する。
							iosuAct_invalidateAccountCache();
							state->av->PrepareLaunch();
							{ std::lock_guard lock(state->statusMutex); state->error.clear(); }
							auto prepared = GameLaunch::Prepare(state->runtime / "games/base");
							Require(prepared.error == GameLaunch::Error::None && CafeSystem::GetForegroundTitleId() == 0x00050000101dbe00ULL && CafeSystem::GetForegroundTitleVersion() == 560, "Minecraft v560 preparation failed");
							const auto textureFiles = state->textures->BeginTitle(CafeSystem::GetForegroundTitleId(), CafeSystem::GetForegroundTitleVersion());
							std::vector<FSCOwnedRedirect> redirects;
							for (const auto& file : textureFiles) redirects.push_back({file.virtualPath, file.path, file.size});
							Require(fscDeviceRedirect_setOwned(state->textures.get(), redirects), "Texture overlay conflicts with another redirect owner");
							Require(fscDeviceRedirect_ownedCount(state->textures.get()) == textureFiles.size(), "Texture overlay registration failed");
							for (const auto& file : textureFiles)
							{
								const auto resolved = fsc_extractFile(file.virtualPath.c_str());
								Require(resolved && resolved->size() == file.size, "Texture overlay resolution failed");
								std::array<uint8_t, SHA256_DIGEST_LENGTH> digest{}; SHA256(resolved->data(), resolved->size(), digest.data());
								constexpr char hex[] = "0123456789abcdef"; std::string hash;
								for (const auto byte : digest) { hash += hex[byte >> 4]; hash += hex[byte & 15]; }
								Require(hash == file.sha256, "Texture overlay resolved unexpected bytes");
							}
							cemuLog_log(LogType::Force, "NativeHost texture overlay: registered={} session={}", textureFiles.size(), state->sessionId.load());
							NativeSettings settings;
							{
								std::lock_guard lock(state->statusMutex);
								settings = state->nativeSettings;
							}
							Require(MinecraftNativeResolution::Begin(CafeSystem::GetForegroundTitleId(), CafeSystem::GetForegroundTitleVersion(), settings.width, settings.height), "Invalid native resolution session");
							if (settings.vsyncFrequency) LatteTiming_setCustomVsyncFrequency(settings.vsyncFrequency);
							else LatteTiming_disableCustomVsyncFrequency();
							state->appliedVsync = settings.vsyncFrequency;
							{ std::lock_guard lock(state->imageMutex); state->images.clear(); }
							cemuLog_log(LogType::Force, "NativeHost session settings: vsync={} internal={}x{}", settings.vsyncFrequency, settings.width, settings.height);
							if (!state->stop && !state->titleStop)
							{
								const Vector2i size{s_windowInfo.phys_width, s_windowInfo.phys_height};
								Require(size.x > 0 && size.y > 0, "Game canvas has no drawable size");
								const bool useOpenGL = GetConfig().graphic_api == kOpenGL;
								state->Phase(useOpenGL ? "opengl" : "vulkan");
								{
									std::lock_guard lock(state->statusMutex);
									state->activeRenderer = useOpenGL ? "OpenGL" : "Vulkan";
								}
								if (useOpenGL)
								{
									openGL = std::make_unique<NativeOpenGL>(state->callbacks.openGLCanvas(), settings.width, settings.height, [state = state] { ++state->glPresents; });
									++state->glContextsCreated;
									g_renderer = std::make_unique<OpenGLRenderer>();
									std::lock_guard lock(state->statusMutex); state->rendererDevice = openGL->DeviceName();
								}
								else
								{
									Require(InitializeGlobalVulkan(), "Vulkan loader is unavailable");
									g_renderer = std::make_unique<VulkanRenderer>();
									const auto renderer = VulkanRenderer::GetInstance();
									const auto limits = renderer->GetPhysicalDevice();
									VkPhysicalDeviceProperties properties{};
									vkGetPhysicalDeviceProperties(limits, &properties);
									Require(settings.width <= properties.limits.maxImageDimension2D && settings.height <= properties.limits.maxImageDimension2D, "Native resolution exceeds Vulkan image limits");
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
									std::lock_guard lock(state->statusMutex); state->rendererDevice = renderer->GetDeviceName();
								}
								g_isGPUInitFinished = false;
								if (!state->stop && !state->titleStop) CafeSystem::LaunchForegroundTitle();
								if (CafeSystem::IsTitleRunning())
								{
									sint32 applied = 0;
									const bool custom = LatteTiming_getCustomVsyncFrequency(applied);
									Require(custom == (settings.vsyncFrequency != 0) && (!custom || applied == settings.vsyncFrequency), "Native VSync was changed during title initialization");
									state->appliedVsync = custom ? applied : 0;
									{ std::lock_guard lock(state->statusMutex); state->rendererDevice = useOpenGL ? openGL->DeviceName() : g_renderer->GetDeviceName(); }
									cemuLog_log(LogType::Force, "NativeHost verified LatteTiming after title initialization: custom={} frequency={}", custom, applied);
								}
								state->Phase("running");
								std::unique_lock lock(state->workMutex);
								state->wake.wait(lock, [&] { return state->stop || state->titleStop; });
							}
						}
					}
					catch (const std::exception& error)
					{
						cemuLog_log(LogType::Force, "NativeHost title launch failed: {}", error.what());
						std::lock_guard lock(state->statusMutex); state->error = error.what();
					}
					// coordinator だけが停止・join・Renderer 解放を行う。UI STA は待たない。
					CafeSystem::ShutdownTitle();
					fscDeviceRedirect_removeOwned(state->textures.get());
					Require(fscDeviceRedirect_ownedCount(state->textures.get()) == 0, "Texture overlay was not released");
					state->textures->EndTitle();
					state->av->EndTitle();
					Require(!CafeSystem::IsTitleRunning() && Latte_GetStopSignal(), "Title threads did not stop");
					ReleaseRenderer();
					MinecraftNativeResolution::End();
					LatteTiming_disableCustomVsyncFrequency();
					state->appliedVsync = 0;
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
							state->titleStop = false;
							s_inputRecoveryPending = false;
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
			if (textureWorker.joinable()) textureWorker.join();
			CafeSystem::Shutdown();
			if (state->textures)
			{
				fscDeviceRedirect_removeOwned(state->textures.get());
				state->textures->EndTitle();
			}
			if (state->av) state->av->EndTitle();
			cemuLog_log(LogType::Force, "NativeHost CafeSystem shutdown complete; rendererPresent={}", bool(g_renderer));
			// GPU 未開始の取消でも、既存 LatteThread_Exit と同じく destructor 中は
			// g_renderer を有効に保つ。内部資源の解放がこの参照を利用する。
			ReleaseRenderer();
			MinecraftNativeResolution::End();
			LatteTiming_disableCustomVsyncFrequency();
			state->appliedVsync = 0;
			{ std::lock_guard workLock(state->workMutex); std::lock_guard statusLock(state->statusMutex); state->inputReady = false; state->accounts.reset(); state->input.reset(); state->av.reset(); }
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
	if (m_impl->textureWorker.joinable()) m_impl->textureWorker.join();
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
		if (!m_impl->state->CanEditSettingsLocked()) return;
		if (m_impl->state->input) m_impl->state->input->CancelCapture();
		m_impl->state->launch = true;
		m_impl->state->titleStop = false;
		m_impl->state->ClearStopConfirmationLocked();
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
bool CemuHost::BeginStopGameConfirmation(uint64_t sessionId)
{
	bool accepted = false;
	{
		std::lock_guard workLock(m_impl->state->workMutex);
		std::lock_guard statusLock(m_impl->state->statusMutex);
		std::lock_guard uiLock(m_impl->state->uiMutex);
		if (m_impl->state->CanStopGameLocked() && sessionId == m_impl->state->sessionId && !m_impl->state->confirmationId)
		{
			m_impl->state->confirmationId = ++m_impl->state->confirmationCounter;
			m_impl->state->confirmationSession = m_impl->state->sessionId;
			m_impl->state->confirmationGeneration = m_impl->state->generation;
			accepted = true;
		}
	}
	m_impl->state->Publish();
	return accepted;
}
void CemuHost::CancelStopGameConfirmation(uint64_t sessionId, uint64_t confirmationId)
{
	{
		std::lock_guard workLock(m_impl->state->workMutex);
		std::lock_guard statusLock(m_impl->state->statusMutex);
		if (confirmationId && m_impl->state->confirmationId == confirmationId && m_impl->state->confirmationSession == sessionId)
			m_impl->state->ClearStopConfirmationLocked();
	}
	m_impl->state->Publish();
}
bool CemuHost::ConfirmStopGame(uint64_t sessionId, uint64_t confirmationId)
{
	const bool accepted = sessionId && confirmationId && m_impl->state->StopTitle("user-confirmed-stop", sessionId, confirmationId);
	if (!accepted) m_impl->state->Publish();
	return accepted;
}
bool CemuHost::HasStopGameConfirmation() const
{
	std::lock_guard lock(m_impl->state->statusMutex);
	return m_impl->state->confirmationId != 0 && m_impl->state->CanStopGameLocked();
}
void CemuHost::RequestStop() { m_impl->state->Stop(); }
void CemuHost::ReportDisplayFailure() { m_impl->state->Fail("Unable to switch launcher display mode"); }
bool CemuHost::IsStopped() const { return m_impl->state->stopped; }
bool CemuHost::IsRunning() const
{
	std::lock_guard lock(m_impl->state->statusMutex);
	return m_impl->state->phase == "running" && !m_impl->state->stop;
}
bool CemuHost::IsTitleStopping() const { return m_impl->state->titleStop && m_impl->state->launch; }
uint64_t CemuHost::SessionId() const { return m_impl->state->sessionId; }
bool CemuHost::HasFailed() const { return m_impl->state->failed; }
std::string CemuHost::StatusJson() const { return m_impl->state->Json(); }

void CemuHost::SaveNativeSettings(std::string_view payload)
{
	auto& state = *m_impl->state;
	NativeSettings settings;
	const bool valid = NativeSettings::Parse(payload, settings);
	{
		std::lock_guard workLock(state.workMutex);
		std::lock_guard statusLock(state.statusMutex);
		if (!state.CanEditSettingsLocked()) state.settingsNotice = "not-ready";
		else if (!valid) state.settingsNotice = "invalid";
		else
		{
			const auto path = state.runtime / "portable/launcher-native-settings.json";
			const auto attributes = GetFileAttributesW(path.c_str());
			if ((attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) || !settings.Save(path))
				state.settingsNotice = "save-failed";
			else
			{
				state.nativeSettings = settings;
				state.settingsNotice = "saved";
			}
		}
	}
	state.Publish();
}

std::string CemuHost::InputCommand(uint64_t requestId, std::string_view payload)
{
	auto& state = *m_impl->state;
	std::lock_guard workLock(state.workMutex);
	std::lock_guard statusLock(state.statusMutex);
	if (!state.input) return "{\"notice\":\"not-ready\"}";
	if (!requestId || requestId <= state.lastInputRequest) return "{\"notice\":\"stale-request\"}";
	state.lastInputRequest = requestId;
	if (payload == "{\"op\":\"cancel\"}") { state.input->CancelCapture(); return state.input->Snapshot(state.CanEditInputLocked() && (state.phase != "running" || !s_windowInfo.app_active)); }
	if (!state.CanEditInputLocked() || (state.phase == "running" && s_windowInfo.app_active)) { state.input->CancelCapture(); return "{\"notice\":\"not-ready\"}"; }
	return state.input->Command(payload);
}

std::string CemuHost::PollInput()
{
	auto& state = *m_impl->state;
	std::lock_guard workLock(state.workMutex);
	std::lock_guard statusLock(state.statusMutex);
	if (!state.input || state.stop || state.failed || (state.phase != "ready" && state.phase != "running")) return "{}";
	if (!state.CanEditInputLocked() || (state.phase == "running" && s_windowInfo.app_active)) state.input->CancelCapture();
	return state.input->Poll();
}

void CemuHost::CancelInputCapture()
{
	auto& state = *m_impl->state;
	std::lock_guard workLock(state.workMutex);
	std::lock_guard statusLock(state.statusMutex);
	if (state.input) state.input->CancelCapture();
}

std::string CemuHost::AvCommand(uint64_t requestId, std::string_view payload)
{
	auto& state = *m_impl->state;
	std::lock_guard workLock(state.workMutex);
	std::lock_guard statusLock(state.statusMutex);
	if (!state.av) return "{\"notice\":\"not-ready\"}";
	if (!requestId || requestId <= state.lastAvRequest) return "{\"notice\":\"stale-request\"}";
	state.lastAvRequest = requestId;
	if (!state.CanEditAudioLocked()) return "{\"notice\":\"not-ready\"}";
	return state.av->Command(payload, state.phase == "running");
}

std::string CemuHost::FriendsCommand(bool refresh)
{
	auto& state = *m_impl->state;
	std::lock_guard workLock(state.workMutex);
	std::lock_guard statusLock(state.statusMutex);
	if (!state.accounts || state.stop || state.failed) return "{\"status\":\"account-not-ready\"}";
	if (!state.friendsStarted)
	{
		if (state.phase != "running") state.accounts->PrepareLaunch();
		FriendsSession::Instance().ConfigureCurrent(true, state.accounts->NetworkName());
		state.friendsStarted = true;
	}
	if (refresh) FriendsSession::Instance().Refresh();
	return FriendsSnapshotJson();
}

std::string CemuHost::ManageFriends(uint64_t requestId, std::string_view payload)
{
	auto& state = *m_impl->state;
	std::lock_guard workLock(state.workMutex);
	std::lock_guard statusLock(state.statusMutex);
	if (!state.accounts || !state.friendsStarted || state.stop || state.failed)
		return "{\"requestId\":" + std::to_string(requestId) + ",\"notice\":\"not-connected\"}";
	return FriendsManagementCommand(requestId, payload);
}

std::string CemuHost::OpenWorldFolder(uint64_t requestId, std::string_view payload, const std::function<bool(const std::wstring&)>& open)
{
	auto& state = *m_impl->state;
	std::unique_lock workLock(state.workMutex); std::unique_lock statusLock(state.statusMutex);
	if (GetCurrentThreadId() != state.uiThread || !state.worlds || !state.accounts || state.stop || state.failed || state.openingWorldFolder) return "{\"notice\":\"not-ready\"}";
	if (!requestId || requestId <= state.lastWorldRequest) return "{\"notice\":\"stale-request\"}";
	state.lastWorldRequest = requestId;
	if (std::chrono::steady_clock::now() < state.nextWorldFolderOpen) return "{\"notice\":\"folder-open-busy\"}";
	return state.worlds->OpenFolder(payload, state.WorldContext(), [&](const fs::path& folder) {
		state.openingWorldFolder = true; state.nextWorldFolderOpen = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		// Shell の COM 呼出し中は Core のロックを外し、起動・Account 変更は Ready gate で止める。
		statusLock.unlock(); workLock.unlock();
		bool opened = false;
		try { opened = open && open(folder.native()); } catch (...) {}
		workLock.lock(); statusLock.lock(); state.openingWorldFolder = false;
		return opened;
	});
}

std::string CemuHost::WorldCommand(uint64_t requestId, std::string_view payload)
{
	auto& state = *m_impl->state;
	std::lock_guard workLock(state.workMutex);
	std::lock_guard statusLock(state.statusMutex);
	if (!state.worlds || !state.accounts || state.stop || state.failed || state.openingWorldFolder) return "{\"notice\":\"not-ready\"}";
	if (!requestId || requestId <= state.lastWorldRequest) return "{\"notice\":\"stale-request\"}";
	state.lastWorldRequest = requestId;
	return state.worlds->Command(payload, state.WorldContext());
}

void CemuHost::TextureCommand(uint64_t requestId, std::string payload, std::function<void(std::string)> complete)
{
	auto state = m_impl->state;
	std::lock_guard workLock(state->workMutex);
	std::lock_guard statusLock(state->statusMutex);
	if (!state->textures || state->stop || state->failed || state->textureBusy || !requestId || requestId <= state->lastTextureRequest)
	{
		complete("{\"notice\":\"not-ready\"}"); return;
	}
	const bool editable = state->CanEditWorldsLocked() && (!state->worlds || state->worlds->IsHealthy());
	state->lastTextureRequest = requestId;
	if (m_impl->textureWorker.joinable()) m_impl->textureWorker.join();
	state->textureBusy = true;
	try
	{
		m_impl->textureWorker = std::thread([state, payload = std::move(payload), complete = std::move(complete), editable] {
			const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			std::string result = "{\"notice\":\"not-ready\"}";
			if (SUCCEEDED(apartment))
			{
				result = state->textures->Command(payload, editable);
				CoUninitialize();
			}
			{
				std::lock_guard workLock(state->workMutex); std::lock_guard statusLock(state->statusMutex); state->textureBusy = false;
			}
			state->callbacks.post([state, complete, result = std::move(result)] {
				if (state->alive && !state->stop) { complete(result); state->Publish(); }
			});
		});
	}
	catch (...) { state->textureBusy = false; throw; }
}

std::string CemuHost::OpenTextureFolder(uint64_t requestId, const std::function<bool(const std::wstring&)>& open)
{
	auto& state = *m_impl->state;
	std::unique_lock workLock(state.workMutex); std::unique_lock statusLock(state.statusMutex);
	if (!state.textures || state.stop || state.failed || state.textureBusy || GetCurrentThreadId() != state.uiThread || !requestId || requestId <= state.lastTextureRequest) return "{\"notice\":\"not-ready\"}";
	state.lastTextureRequest = requestId;
	if (std::chrono::steady_clock::now() < state.nextTextureFolderOpen) return "{\"notice\":\"folder-open-busy\"}";
	state.nextTextureFolderOpen = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	state.textureBusy = true; statusLock.unlock(); workLock.unlock();
	const auto result = state.textures->OpenFolder([&](const fs::path& path) { return open(path.native()); });
	workLock.lock(); statusLock.lock(); state.textureBusy = false; return result;
}

std::string CemuHost::PreviewWorldImage(uint64_t requestId, std::string_view payload, const std::wstring& selectedFile)
{
	auto& state = *m_impl->state;
	std::lock_guard workLock(state.workMutex);
	std::lock_guard statusLock(state.statusMutex);
	if (!state.worlds || !state.accounts || state.stop || state.failed || state.openingWorldFolder) return "{\"notice\":\"not-ready\"}";
	if (!requestId || requestId <= state.lastWorldRequest) return "{\"notice\":\"stale-request\"}";
	state.lastWorldRequest = requestId;
	return state.worlds->PreviewImage(payload, selectedFile, state.WorldContext());
}

std::string CemuHost::PreviewWorldImport(uint64_t requestId, std::string_view payload, const std::wstring& selected, std::string_view part)
{
	auto& state = *m_impl->state;
	std::lock_guard workLock(state.workMutex); std::lock_guard statusLock(state.statusMutex);
	if (!state.worlds || !state.accounts || state.stop || state.failed || state.openingWorldFolder) return "{\"notice\":\"not-ready\"}";
	if (!requestId || requestId <= state.lastWorldRequest) return "{\"notice\":\"stale-request\"}";
	state.lastWorldRequest = requestId;
	return state.worlds->PreviewImport(payload, selected, part, state.WorldContext());
}

std::string CemuHost::AccountCommand(uint64_t requestId, std::string_view payload)
{
	auto& state = *m_impl->state;
	std::lock_guard workLock(state.workMutex);
	std::lock_guard statusLock(state.statusMutex);
	if (!state.accounts || state.stop || state.failed) return "{\"notice\":\"not-ready\"}";
	if (!requestId || requestId <= state.lastAccountRequest) return "{\"notice\":\"stale-request\"}";
	state.lastAccountRequest = requestId;
	const bool wasTesting = state.accounts->IsTesting();
	const auto result = state.accounts->Command(payload, state.CanEditSettingsLocked());
	if (state.friendsStarted) FriendsSession::Instance().ConfigureCurrent(true, state.accounts->NetworkName());
	if (!wasTesting && state.accounts->IsTesting()) { state.networkTest = true; state.wake.notify_all(); }
	return result;
}

std::string CemuHost::PreviewAccountZip(uint64_t requestId, const std::wstring& selectedFile)
{
	auto& state = *m_impl->state;
	std::lock_guard workLock(state.workMutex);
	std::lock_guard statusLock(state.statusMutex);
	if (!state.accounts || state.stop || state.failed) return "{\"notice\":\"not-ready\"}";
	if (!requestId || requestId <= state.lastAccountRequest) return "{\"notice\":\"stale-request\"}";
	state.lastAccountRequest = requestId;
	return state.accounts->PreviewZip(selectedFile, state.CanEditSettingsLocked());
}

std::string CemuHost::ImportCustomNetwork(uint64_t requestId, const std::wstring& selectedFile, bool replaceExisting)
{
	auto& state = *m_impl->state;
	std::lock_guard workLock(state.workMutex); std::lock_guard statusLock(state.statusMutex);
	if (!state.accounts || !requestId || requestId <= state.lastAccountRequest) return "{\"notice\":\"not-ready\"}";
	state.lastAccountRequest = requestId;
	const auto result = state.accounts->ImportCustom(selectedFile, state.CanEditSettingsLocked(), replaceExisting);
	if (state.friendsStarted) FriendsSession::Instance().ConfigureCurrent(true, state.accounts->NetworkName());
	return result;
}

void CemuHost::RefreshSettingsInfo()
{
	auto& state = *m_impl->state;
	bool changed = false;
	{
		std::lock_guard workLock(state.workMutex);
		std::lock_guard statusLock(state.statusMutex);
		if (state.corePhase != "ready" || (state.phase != "ready" && state.phase != "running") || state.stop || state.failed)
			return;
		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		writer.StartObject();
		const auto& config = GetConfig();
		const std::array<const char*, 4> backends{"DirectSound", "XAudio2 2.7", "XAudio2", "Cubeb"};
		writer.Key("audioBackend"); writer.String(config.audio_api >= 0 && config.audio_api < backends.size() ? backends[config.audio_api] : "Unknown");
		const auto deviceName = [&](const std::wstring& identifier) {
			if (identifier.empty()) return std::string("未選択");
			if (identifier == L"default") return std::string("既定のデバイス");
			return boost::nowide::narrow(identifier);
		};
		writer.Key("tvDevice"); writer.String(deviceName(config.tv_device).c_str());
		writer.Key("padDevice"); writer.String(deviceName(config.pad_device).c_str());
		writer.Key("tvVolume"); writer.Int(config.tv_volume);
		writer.Key("padVolume"); writer.Int(config.pad_volume);
		writer.Key("vpadAssigned");
		const auto controller = InputManager::instance().get_vpad_controller(0);
		writer.Bool(bool(controller));
		writer.Key("profile"); writer.String(controller ? controller->get_profile_name().c_str() : "未割当");
		writer.Key("controllers"); writer.StartArray();
		if (controller)
		{
			pugi::xml_document profile;
			profile.load_file((state.runtime / "portable/controllerProfiles/controller0.xml").c_str());
			// 実効 mapping の参照だけを読み、provider や SDL を追加初期化しない。
			std::set<std::shared_ptr<ControllerBase>> devices;
			for (size_t mapping = 0; mapping <= controller->get_highest_mapping_id(); ++mapping)
				if (auto device = controller->get_mapping_controller(mapping)) devices.insert(std::move(device));
			for (const auto& device : devices)
			{
				writer.StartObject();
				writer.Key("api"); writer.String(device->api_name().data(), static_cast<rapidjson::SizeType>(device->api_name().size()));
				std::string name = "取得できません";
				for (const auto& configured : profile.child("emulated_controller").children("controller"))
					if (device->uuid() == configured.child_value("uuid") && device->api_name() == configured.child_value("api"))
						name = configured.child_value("display_name");
				writer.Key("name"); writer.String(name.c_str());
				writer.Key("connected"); writer.Bool(device->is_connected());
				writer.EndObject();
			}
		}
		writer.EndArray();
		if (state.av) { writer.Key("av"); const auto snapshot = state.av->Snapshot(); writer.RawValue(snapshot.data(), snapshot.size(), rapidjson::kObjectType); }
		if (state.input) { writer.Key("input"); const auto snapshot = state.input->Snapshot(state.CanEditInputLocked() && (state.phase != "running" || !s_windowInfo.app_active)); writer.RawValue(snapshot.data(), snapshot.size(), rapidjson::kObjectType); }
		writer.EndObject();
		changed = state.settingsInfo != buffer.GetString();
		state.settingsInfo = buffer.GetString();
	}
	if (changed) state.Publish();
}
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
	LatteOverlay_setControlInset(!fullscreen && visible ? MulDiv(56, dpi, 96) : 0);
	s_windowInfo.app_active = visible && focused && IsRunning() && !m_impl->state->titleStop && !HasStopGameConfirmation();
	if (!s_windowInfo.app_active) InputMessage(WM_CANCELMODE, 0, 0);
}
uint64_t CemuHost::StopConfirmationId() const
{
	std::lock_guard lock(m_impl->state->statusMutex);
	return m_impl->state->confirmationId;
}

void CemuHost::InputMessage(UINT message, WPARAM key, LPARAM value)
{
	// マウスは既存 VPAD の image-area 変換に渡す。KBM の割当は追加しない。
	if (m_impl->state->inputReady && (message == WM_LBUTTONDOWN || message == WM_MOUSEMOVE || message == WM_LBUTTONUP || message == WM_CAPTURECHANGED || message == WM_KILLFOCUS || message == WM_CANCELMODE))
	{
		auto& mouse = InputManager::instance().m_main_mouse;
		std::unique_lock lock(mouse.m_mutex);
		if (message == WM_CAPTURECHANGED || message == WM_KILLFOCUS || message == WM_CANCELMODE)
			mouse.left_down = mouse.left_down_toggle = false;
		else if (message == WM_LBUTTONUP)
			mouse.left_down = false;
		else if (s_windowInfo.app_active && IsRunning() && !m_impl->state->titleStop)
		{
			const glm::ivec2 point{GET_X_LPARAM(value), GET_Y_LPARAM(value)};
			int x = 0, y = 0, width = 0, height = 0;
			LatteRenderTarget_getScreenImageArea(&x, &y, &width, &height, nullptr, nullptr, false);
			const bool inside = width > 0 && height > 0 && point.x >= x && point.y >= y && point.x < x + width && point.y < y + height;
			mouse.position = point;
			if (!inside) mouse.left_down = mouse.left_down_toggle = false;
			else if (message == WM_LBUTTONDOWN) mouse.left_down = mouse.left_down_toggle = true;
		}
	}
	if (message == WM_KILLFOCUS || message == WM_CANCELMODE) { s_windowInfo.set_keystatesup(); s_windowInfo.app_active = false; return; }
	if (message == WM_SETFOCUS) { s_windowInfo.app_active = IsRunning() && !m_impl->state->titleStop && !HasStopGameConfirmation(); return; }
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
		return !s_windowInfo.app_active || s_inputRecoveryPending;
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
		const bool ready = state->Dispatch([state] { return g_renderer && state->callbacks.prepareCanvas(GetConfig().graphic_api == kOpenGL); }, true);
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
				state->RefreshInputConnection();
				const auto title = idle ? L"MCWiiU Launcher" : loading ? L"MCWiiU Launcher - Loading" :
					boost::nowide::widen(fmt::format("MCWiiU Launcher - FPS: {:.2f} [{}] [TitleId: 00050000-101dbe00] [JP v560]", fps, state->activeRenderer));
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
