#include "CemuHost.h"
#include "Cafe/OS/libs/swkbd/HostInput.h"
#include "MiiDatabase.h"
#include "MiiInitializationStorage.h"
#include "Cafe/OS/MiiInitialization.h"
#include "Common/FileStream.h"
#include "NativeSettings.h"
#include "LauncherPaths.h"
#include "SessionHistory.h"
#include "UpdateSecurity.h"
#include "InputService.h"
#include "AvSettings.h"
#include "AccountService.h"
#include "FriendsService.h"
#include "WorldService.h"
#include "TextureService.h"
#include "GraphicPackService.h"
#include "DirectoryChangeMonitor.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/Account/Account.h"
#include "Cafe/TitleList/SaveInfo.h"
#include "Cafe/TitleList/MinecraftTitle.h"
#include "Cemu/nex/FriendsSession.h"
#include "NativeOpenGL.h"

#include "Cemu/Startup.h"
#include "Cemu/GameLaunch.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/IOSU/legacy/iosu_act.h"
#include "Cafe/OS/libs/sysapp/sysapp.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Core/LatteTiming.h"
#include "Cafe/HW/Latte/Core/LatteIdleWait.h"
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
#include <fstream>
#include <openssl/sha.h>

std::atomic_bool g_isGPUInitFinished{false};

// Native entry は CLI の console 経路を使用しない。
void requireConsole()
{
	throw std::runtime_error("Cemu CLI console operations are unavailable in LauncherShell");
}

namespace
{
#ifdef MCWIIU_JPN_MII_INITIALIZATION
	constexpr bool s_enableMiiInitialization = true;
#else
	constexpr bool s_enableMiiInitialization = false;
#endif
	void LogMiiCleanup(const MiiInitializationStorage::CleanupResult& result)
	{
		if (result.bytes || result.pending)
			cemuLog_log(LogType::Force, "Mii initialization cleanup: reclaimedBytes={} pendingSessions={} detail={}", result.bytes, result.pending, result.reason);
	}
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

	MiiDatabase::Inspection ValidateMinecraftMiiData(const fs::path& mlc, uint64_t miiTitleId, bool allowInitialization)
	{
		// v560 は FFL 初期化失敗後も Mii 一覧を参照する。欠落時は PPC 実行前に止め、原本は修復しない。
		const auto check = [&](const fs::path& relative, std::string_view magic, uintmax_t minimumSize) {
			const auto path = mlc / relative;
			std::error_code error;
			const bool regular = fs::is_regular_file(path, error);
			const auto size = regular && !error ? fs::file_size(path, error) : 0;
			std::array<char, 4> header{};
			std::ifstream file(path, std::ios::binary);
			const bool readable = bool(file.read(header.data(), header.size()));
			if (error || !regular || size < minimumSize || !readable || std::string_view(header.data(), header.size()) != magic)
			{
				cemuLog_log(LogType::Force, "NativeHost Mii prerequisite unavailable: {}", _pathToUtf8(relative));
				throw std::runtime_error(fmt::format("Minecraft の起動に必要な Mii システムデータが不足しているか読み取れません: {}。使用する MLC に、所有する Wii U の FFL リソースと初期化済み Mii データベースを用意してください。既存データは変更していません。", _pathToUtf8(relative)));
			}
		};
		const fs::path resources = "sys/title/0005001b/10056000/content";
		// 埋込み FFL が最初に読むヘッダー領域だけを確認し、全ファイルのハッシュ走査は行わない。
		check(resources / "FFLResMiddle.dat", "FFRA", 18944);
		check(resources / "FFLResHigh.dat", "FFRA", 18944);
		const fs::path database = fmt::format("usr/save/{:08x}/{:08x}/user/common/db", uint32_t(miiTitleId >> 32), uint32_t(miiTitleId));
		const auto inspection = MiiDatabase::Inspect(mlc, miiTitleId);
		if (inspection.HasBlockingError()) throw std::runtime_error("Mii data is invalid or unreadable. Existing files were preserved.");
		if (!inspection.HasRequiredDatabases() && !(allowInitialization && inspection.NeedsInitialization()))
		{
			check(database / "FFL_HDB.dat", "FFHA", 44032);
			check(database / "FFL_ODB.dat", "FFOC", 276544);
		}
		return inspection;
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
		std::atomic_bool titleStop{false}, titleRunning{false};
		std::atomic_bool miiInitializing{false}, miiGuestExited{false}, miiRestartRequired{false};
		std::atomic_uint64_t miiGeneration{0}, miiInitializationCount{0};
		std::atomic_int32_t miiReturnStatus{0};
		std::atomic_bool inputReady{false}, networkTest{false};
		std::atomic_uint64_t sessionId{0}, commonInitCount{0}, titleShutdownCount{0};
		std::atomic_uint64_t vulkanInstance{0}, vulkanDevice{0}, vulkanSurface{0}, surfaceCreates{0};
		std::atomic_uint64_t glContextsCreated{0}, glContextsDestroyed{0}, glPresents{0};
		std::atomic_uint64_t presentationSession{0}, presentationBinding{0}, presentationChain{0};
		std::atomic_bool firstFrameQueued{false}, firstFrameVisible{false}, freshFrameRequired{false};
		uint64 vulkanSession = 0; // hook の有効期間は coordinator と GPU join の間に限定する。
		std::string activeRenderer, rendererDevice;
		std::string corePhase = "initializing", titlePhase = "idle", stopReason;
		std::atomic_uint64_t inputReconnectCount{0};
		std::atomic_uint64_t presents{0}, acquires{0}, swapchainCreates{0}, swapchainDestroys{0}, surfaceDestroys{0}, gameListRevision{0};
		std::atomic<double> fps{0};
		std::atomic_uint32_t swapchainWidth{0}, swapchainHeight{0};
		std::string phase = "idle", error;
		fs::path dataRoot;
		bool gameInstalled = false, launchRegionSupported = false, nativeSettingsHealthy = true;
		std::shared_ptr<GameInstallationService> installation;
		std::shared_ptr<SessionHistory> history;
		std::atomic_bool installationRequested{false};
		std::atomic_bool installationCancelled{false};
		bool installationBusy = false;
		std::atomic_uint64_t managerGeneration{0}, managerTitleId{0};
		GameInstallationService::Request installationRequest;
		std::optional<GameInstallationService::Progress> installationQueueProgress;
		NativeSettings nativeSettings;
		std::unique_ptr<InputService> input;
		std::unique_ptr<AvSettings> av;
		std::unique_ptr<AccountService> accounts;
		std::unique_ptr<WorldService> worlds;
		std::unique_ptr<TextureService> textures;
		bool textureBusy = false;
		std::unique_ptr<GraphicPackService> graphicPacks;
		bool graphicPackBusy = false;
		uint64_t lastGraphicPackRequest = 0;
		std::chrono::steady_clock::time_point nextGraphicPackFolderOpen{};
		uint64_t lastTextureRequest = 0;
		std::chrono::steady_clock::time_point nextTextureFolderOpen{};
		uint64_t lastWorldRequest = 0;
		bool openingWorldFolder = false;
		std::chrono::steady_clock::time_point nextWorldFolderOpen{};
		uint64_t lastAccountRequest = 0;
		bool friendsStarted = false;
		DirectoryChangeMonitor graphicPackMonitor, textureMonitor, worldMonitor;
		uint64_t graphicPackRevision = 0, textureRevision = 0, worldRevision = 0;
		bool graphicPackReloadPending = false;
		bool graphicPackSynchronizing = false;
		std::array<uint64_t, 3> friendsVersion{};
		uint64_t lastAvRequest = 0;
		uint64_t lastInputRequest = 0;
		std::string settingsNotice = "loading", settingsInfo = "{}";
		std::mutex imageMutex;
		std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, uint64_t> images;
		std::atomic_uint32_t appliedVsync{0};
		int controllerModifier = -1, controllerExit = -1;

		HostState(HWND rootWindow, HWND gameWindow, CemuHost::Callbacks ui)
			: root(rootWindow), canvas(gameWindow), callbacks(std::move(ui)) {}

		bool CanPresent(uint64 session) const
		{
			return session && presentationSession == session && sessionId == session && alive && !stop && !titleStop && !miiInitializing;
		}
		void FirstFrame(uint64 session, uint64 binding)
		{
			const auto output = s_windowInfo.hostOutputState.load();
			if (!(output & 1) || (freshFrameRequired && s_windowInfo.hostOutputFrame != output)) return;
			if (!CanPresent(session) || firstFrameVisible || presentationBinding != binding || firstFrameQueued.exchange(true)) return;
			cemuLog_log(LogType::Force, "NativeHost First Present: session={} binding={}", session, binding);
			Dispatch([state = shared_from_this(), session, binding, output] {
				if (s_windowInfo.hostOutputState != output || !state->CanPresent(session) || state->presentationBinding != binding || state->firstFrameVisible ||
					!IsWindow(state->root) || !IsWindow(state->canvas))
				{
					cemuLog_log(LogType::Force, "NativeHost stale First Present rejected: session={} binding={}", session, binding);
					return false;
				}
				if (!state->callbacks.firstFrame)
				{
					state->Fail("First frame callback is unavailable");
					return false;
				}
				if (!state->callbacks.firstFrame(session)) return false;
				state->freshFrameRequired = false;
				state->firstFrameVisible = true;
				cemuLog_log(LogType::Force, "NativeHost Black Screen Released: session={} binding={}", session, binding);
				return true;
			}, false);
		}
		void BindPresentationChain(VkSwapchainKHR chain)
		{
			presentationChain = reinterpret_cast<uintptr_t>(chain);
			++presentationBinding;
			firstFrameQueued = false;
		}

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

		bool launcherUpdate = false;
		bool CanStopGameLocked() const
		{
			return corePhase == "ready" && ((phase == "running" && titlePhase == "running") || miiInitializing) && launch && !stop && !titleStop && !stopped && !failed;
		}
		bool CanEditSettingsLocked() const
		{
			return CanEditWorldsLocked() && (!worlds || worlds->IsHealthy()) && (!textures || textures->IsHealthy()) && (!graphicPacks || graphicPacks->IsHealthy());
		}
		bool CanAccessWorldDataLocked() const
		{
			return !launcherUpdate && !installationBusy && !graphicPackBusy && !textureBusy && !openingWorldFolder && corePhase == "ready" && !stop && !titleStop && !stopped && !failed && !miiInitializing && !miiRestartRequired && !confirmationId && (!input || input->IsHealthy()) && (!av || av->IsHealthy()) && (!accounts || (accounts->IsHealthy() && !accounts->IsTesting()));
		}
		bool CanEditWorldsLocked() const
		{
			return CanAccessWorldDataLocked() && phase == "ready" && !launch;
		}
		bool CanManageWorldsLocked() const
		{
			// launch は通常実行中も true。起動・停止途中と Mii 初期化は許可しない。
			return CanAccessWorldDataLocked() && ((phase == "ready" && !launch) || (phase == "running" && titlePhase == "running" && titleRunning && launch));
		}
		bool RegionManagersAvailable() const
		{
			const auto installed = installation->GetSnapshot();
			return !installationBusy && installed.state != GameInstallationService::State::RecoveryRequired && installed.state != GameInstallationService::State::Error &&
				MinecraftTitle::IsBase(installed.baseTitleId) && managerTitleId == installed.baseTitleId;
		}
		void RebindManagersLocked(const GameInstallationService::Snapshot& installed)
		{
			Require(!textureBusy && !openingWorldFolder, "Managers are still in use");
			const bool known = installed.state != GameInstallationService::State::RecoveryRequired && installed.state != GameInstallationService::State::Error && MinecraftTitle::IsBase(installed.baseTitleId);
			const auto titleId = known ? installed.baseTitleId : 0;
			std::unique_ptr<WorldService> nextWorlds;
			if (titleId)
			{
				// 既存 JPN の保管・復旧記録は元の場所で保持する。他地域の記録は共有しない。
				const auto config = titleId == 0x00050000101dbe00ULL ? dataRoot : dataRoot / ".launcher-world-regions" / fmt::format("{:016x}", titleId);
				LauncherPaths::Get().CheckPath(config); fs::create_directories(config); LauncherPaths::Get().CheckPath(config);
				nextWorlds = std::make_unique<WorldService>(config, SaveInfo(installed.saveTitleId).GetPath(), titleId, (managerGeneration + 1) << 32);
			}
			textures->Rebind(known ? installed.basePath / "content/Common" : fs::path{}, known ? installed.updatePath / "content/Common" : fs::path{});
			worlds = std::move(nextWorlds); managerTitleId = titleId; ++managerGeneration;
		}
		GameInstallationService::Snapshot InstallationStatusLocked() const
		{
			auto result = installation->GetSnapshot();
			if (installationQueueProgress)
			{
				result.progress = *installationQueueProgress;
				if (result.progress.completion == GameInstallationService::Completion::Running)
				{
					result.ready = result.launchSupported = false;
					result.state = result.progress.operation == GameInstallationService::Operation::ChangeRegion ? GameInstallationService::State::ChangingRegion :
						result.progress.operation == GameInstallationService::Operation::Uninstall || result.progress.operation == GameInstallationService::Operation::RemoveDLC ? GameInstallationService::State::Uninstalling :
						result.progress.operation == GameInstallationService::Operation::ReinstallBase || result.progress.operation == GameInstallationService::Operation::ReinstallUpdate ? GameInstallationService::State::Reinstalling : GameInstallationService::State::Installing;
				}
			}
			return result;
		}
		WorldService::Context WorldContext() const
		{
			WorldService::Context result{ActiveSettings::GetPersistentId(), accounts->Revision(), CanManageWorldsLocked() && RegionManagersAvailable(), {}, CanEditWorldsLocked() && RegionManagersAvailable()};
			for (const auto& account : Account::GetAccounts())
				result.accounts.push_back({account.GetPersistentId(), boost::nowide::narrow(account.GetMiiName().data(), account.GetMiiName().size())});
			return result;
		}
		bool CanEditLiveSettingsLocked() const
		{
			return !launcherUpdate && corePhase == "ready" && (phase == "ready" || phase == "running") && !stop && !titleStop && !stopped && !failed && !confirmationId;
		}
		bool CanEditInputLocked() const { return CanEditLiveSettingsLocked() && input && input->IsHealthy(); }
		bool CanEditAudioLocked() const { return CanEditLiveSettingsLocked() && av && av->IsHealthy(); }
		bool CanAcknowledgeTexturesLocked() const { return !installationBusy && RegionManagersAvailable() && CanEditLiveSettingsLocked() && textures && !textureBusy; }
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
				writer.Key("gameInstalled"); writer.Bool(gameInstalled);
				const auto installed = InstallationStatusLocked();
				writer.Key("installationState"); writer.String(GameInstallationService::StateName(installed.state));
				writer.Key("installationBusy"); writer.Bool(installationBusy);
				writer.Key("gameRegion"); writer.Int(static_cast<int>(installed.region));
				writer.Key("gameRegionName"); writer.String(MinecraftTitle::RegionName(installed.baseTitleId));
				writer.Key("gameTitleId"); writer.String(fmt::format("{:016x}", installed.baseTitleId).c_str());
				writer.Key("managerGeneration"); writer.Uint64(managerGeneration);
				writer.Key("canManageGameData"); writer.Bool(nativeSettingsHealthy && CanEditSettingsLocked() && installed.state != GameInstallationService::State::RecoveryRequired && installed.state != GameInstallationService::State::Error);
				writer.Key("launchUnavailableReason"); writer.String(installationBusy ? "game-data-busy" : !nativeSettingsHealthy ? "native-settings-invalid" : !gameInstalled ? "game-not-installed" : !launchRegionSupported ? "game-data-unavailable" : "");
				writer.Key("canLaunch"); writer.Bool(installed.launchSupported && gameInstalled && launchRegionSupported && nativeSettingsHealthy && !graphicPackSynchronizing && CanEditSettingsLocked());
				writer.Key("canStopGame"); writer.Bool(CanStopGameLocked());
				writer.Key("stopConfirmationId"); writer.Uint64(confirmationId);
				writer.Key("stopConfirmationSessionId"); writer.Uint64(confirmationSession);
				writer.Key("canEditAccounts"); writer.Bool(CanEditSettingsLocked() && accounts != nullptr);
				writer.Key("canEditWorlds"); writer.Bool(RegionManagersAvailable() && CanManageWorldsLocked() && worlds && worlds->IsHealthy() && (!textures || textures->IsHealthy()) && (!graphicPacks || graphicPacks->IsHealthy()));
				writer.Key("canRecoverWorlds"); writer.Bool(RegionManagersAvailable() && CanEditWorldsLocked() && worlds != nullptr);
				writer.Key("canEditTextures"); writer.Bool(RegionManagersAvailable() && CanEditWorldsLocked() && textures && (!worlds || worlds->IsHealthy()));
				writer.Key("canAcknowledgeTextureWarnings"); writer.Bool(CanAcknowledgeTexturesLocked());
				writer.Key("canEditGraphicPacks"); writer.Bool(CanEditSettingsLocked() && bool(graphicPacks));
				writer.Key("textureBusy"); writer.Bool(textureBusy);
				writer.Key("nativeExitConfirmation"); writer.Bool(true);
				writer.Key("canEditNativeSettings"); writer.Bool(nativeSettingsHealthy && CanEditSettingsLocked());
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
			writer.Key("presentationSession"); writer.Uint64(presentationSession);
			writer.Key("firstFrameVisible"); writer.Bool(firstFrameVisible);
			writer.Key("miiInitializing"); writer.Bool(miiInitializing);
			writer.Key("miiInitializationCount"); writer.Uint64(miiInitializationCount);
			writer.Key("miiRestartRequired"); writer.Bool(miiRestartRequired);
			writer.Key("presentationSwapchain"); writer.Uint64(presentationChain);
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
		std::atomic_bool publishPending{false};
		void Publish()
		{
			// UI 待ちの通知は最新 snapshot に集約し、変更時のキュー蓄積を防ぐ。
			if (publishPending.exchange(true)) return;
			Dispatch([self = shared_from_this()] {
				self->publishPending = false;
				self->callbacks.status(self->Json());
				return true;
			}, false, false);
		}
		void Phase(std::string value)
		{
			{
				std::lock_guard lock(statusMutex);
				if (titleStop && (value == "preparing" || value == "vulkan" || value == "running")) return;
				phase = std::move(value);
				if (phase == "preparing") titlePhase = "preparing";
				if (phase == "vulkan") titlePhase = "starting";
				if (phase == "running")
				{
					titlePhase = "running";
					titleRunning = CafeSystem::IsTitleRunning();
					if (history && !stop && CafeSystem::IsTitleRunning()) history->Begin(sessionId);
				}
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
				if (history) history->End(sessionId);
				titleStop = true;
				titlePhase = "stopping";
				phase = "title-stopping";
				stopReason = std::move(reason);
				presentationSession = 0;
				firstFrameVisible = false;
				CancelTitleUi();
			}
			const auto session = sessionId.load();
			Dispatch([state = shared_from_this(), session] {
				if (state->sessionId != session || !state->titleStop) return false;
				if (state->callbacks.titleStopping) state->callbacks.titleStopping(session);
				return true;
			}, false, false);
			Publish();
			wake.notify_all();
			return true;
		}
		void Stop()
		{
			{
				std::lock_guard workLock(workMutex);
				std::lock_guard statusLock(statusMutex);
				if (history) history->End(sessionId);
				stop = true;
				presentationSession = 0;
				firstFrameVisible = false;
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
			if (miiInitializing)
			{
				const auto result = MiiInitialization::GetResult();
				const auto status = CafeSystem::GetForegroundTitleReturnStatus();
				if (result.generation == miiGeneration && status)
				{
					miiReturnStatus = *status;
					miiGuestExited = true;
					wake.notify_all();
				}
				return;
			}
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
	void ValidateStorageSettings()
	{
		const auto& paths = LauncherPaths::Get();
		const auto settingsPath = paths.data / "settings.xml";
		paths.CheckPath(settingsPath);
		Require(fs::file_size(settingsPath) <= 4 * 1024 * 1024, "Settings file is too large; original preserved");
		pugi::xml_document settings;
		Require(settings.load_file(settingsPath.c_str(), pugi::parse_full), "Invalid settings.xml; original preserved");
		for (const auto& node : settings.children()) Require(node.type() != pugi::node_doctype, "Settings must not contain a doctype; original preserved");
		const auto config = settings.child("content");
		Require(config && !config.next_sibling("content"), "Invalid settings.xml structure; original preserved");
		Require(!config.child("mlc_path").next_sibling("mlc_path") && !config.child("GamePaths").next_sibling("GamePaths"), "Duplicate storage settings; original preserved");
		const auto mlc = _utf8ToPath(config.child_value("mlc_path"));
		Require(mlc.empty() || (mlc.is_absolute() && _wcsicmp(mlc.lexically_normal().c_str(), (paths.data / "mlc01").c_str()) == 0),
			"External MLC path is not allowed; settings.xml was preserved. Select Data/mlc01 before retrying");
		const auto checkReference = [&](const char* value) {
			const auto path = _utf8ToPath(value);
			Require(paths.ContainsData(path), "External game path is not allowed; original settings/cache preserved");
			paths.CheckPath(path);
			bool titlePath = false;
			for (auto current = path.lexically_normal(); !current.empty(); current = current.parent_path())
			{
				if (_wcsicmp(current.c_str(), (paths.data / "mlc01/usr/title").lexically_normal().c_str()) == 0 ||
					_wcsicmp(current.c_str(), (paths.data / "mlc01/sys/title").lexically_normal().c_str()) == 0) { titlePath = true; break; }
				if (current == current.root_path()) break;
			}
			Require(titlePath, "Legacy game paths require explicit MLC installation; original settings/cache preserved");
		};
		for (const auto& entry : config.child("GamePaths").children()) checkReference(entry.child_value());
		for (const auto& entry : config.child("GameCache").children()) checkReference(entry.child_value("path"));
		const auto cache = paths.cache / "title_list_cache.xml";
		paths.CheckPath(cache);
		if (fs::exists(cache))
		{
			Require(fs::file_size(cache) <= 16 * 1024 * 1024, "Title cache is too large; original preserved");
			pugi::xml_document titles;
			Require(titles.load_file(cache.c_str()) && titles.child("title_list"), "Invalid title cache; original preserved");
			for (const auto& title : titles.child("title_list").children("title")) checkReference(title.child_value("path"));
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
	}
	VKAPI_ATTR VkResult VKAPI_CALL Present(VkQueue queue, const VkPresentInfoKHR* info)
	{
		auto& state = *s_vulkanHost;
		const auto session = state.vulkanSession;
		const auto binding = state.presentationBinding.load();
		const auto result = s_present(queue, info);
		if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && state.presents.fetch_add(1) == 0)
			cemuLog_log(LogType::Force, "NativeHost first vkQueuePresentKHR: result={} GPU thread={}", result, GetCurrentThreadId());
		if (!state.firstFrameVisible && state.CanPresent(session) && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR))
		{
			for (uint32 index = 0; index < info->swapchainCount; ++index)
			{
				const auto itemResult = info->pResults ? info->pResults[index] : result;
				if (reinterpret_cast<uintptr_t>(info->pSwapchains[index]) != state.presentationChain ||
					(itemResult != VK_SUCCESS && itemResult != VK_SUBOPTIMAL_KHR)) continue;
				// 初回は LatteThread の DrawEmptyFrame が明示 clear した現在の画像。
				// 呼出し成功だけで、未定義の画像や別 Surface の Present を公開しない。
				const auto renderer = VulkanRenderer::GetInstance();
				const auto& chain = renderer->GetChainInfo(true);
				if (chain.m_swapchain == info->pSwapchains[index] && chain.hasDefinedSwapchainImage &&
					chain.swapchainImageIndex == info->pImageIndices[index]) state.FirstFrame(session, binding);
			}
		}
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
			if (reinterpret_cast<uintptr_t>(device) == s_vulkanHost->vulkanDevice &&
				reinterpret_cast<uintptr_t>(info->surface) == s_vulkanHost->vulkanSurface)
				s_vulkanHost->BindPresentationChain(*chain);
		}
		cemuLog_log(LogType::Force, "NativeHost swapchain create: {}x{} result={} device=0x{:x} surface=0x{:x}",
			info->imageExtent.width, info->imageExtent.height, result, reinterpret_cast<uintptr_t>(device), reinterpret_cast<uintptr_t>(info->surface));
		return result;
	}
	VKAPI_ATTR void VKAPI_CALL DestroySwapchain(VkDevice device, VkSwapchainKHR chain, const VkAllocationCallbacks* allocator)
	{
		if (reinterpret_cast<uintptr_t>(chain) == s_vulkanHost->presentationChain)
			s_vulkanHost->BindPresentationChain(VK_NULL_HANDLE);
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
		state.vulkanSession = state.sessionId;
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
		state->BindPresentationChain(VK_NULL_HANDLE);
		state->vulkanSession = 0;
		state->vulkanInstance = state->vulkanDevice = state->vulkanSurface = 0;
		g_isGPUInitFinished = false;
	}
	void StopMiiTitle()
	{
		// joinをUIやguest自身で待たない。停止が遅れた場合も所有リソースを解放しない。
		auto stopped = std::async(std::launch::async, [] { CafeSystem::ShutdownTitle(); });
		if (stopped.wait_for(std::chrono::seconds(15)) != std::future_status::ready)
		{
			state->miiRestartRequired = true;
			{ std::lock_guard lock(state->statusMutex); state->error = "Mii data initialization could not stop. Restart Launcher."; }
			state->Phase("mii-restart-required");
		}
		stopped.get();
	}

	void Run()
	{
		const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		bool commonStarted = false;
		std::unique_ptr<MiiInitializationStorage> miiStorage;
		bool miiAttempted = false, miiTimedOut = false;
		std::chrono::steady_clock::time_point miiDeadline;
		const auto cancelMii = [&] {
			if (state->stop || state->titleStop) return true;
			if (std::chrono::steady_clock::now() >= miiDeadline) { miiTimedOut = true; return true; }
			return false;
		};
		try
		{
			Require(SUCCEEDED(apartment), "Core coordinator requires MTA");
			state->Phase("validating");
			const auto& paths = LauncherPaths::Get();
			state->dataRoot = paths.data;
			const auto& data = paths.data;
			ValidateStorageSettings();
			LogMiiCleanup(MiiInitializationStorage::Recover(data));
			const auto installed = state->installation->GetSnapshot();
			Require(installed.state != GameInstallationService::State::RecoveryRequired, "Game installation recovery is required before starting Core");
			{
				std::lock_guard lock(state->statusMutex);
				state->gameInstalled = installed.ready;
				state->launchRegionSupported = installed.launchSupported;
			}
			// 中断した取引がある場合は、Default Account 作成を含む共通初期化より前に止める。
			Require(!fs::exists(data / ".launcher-account-transaction"), "Account recovery is required before starting Core");
			const auto nativePath = data / "launcher-native-settings.json";
			if (fs::exists(nativePath)) NoReparsePoints(nativePath);
			{
				std::lock_guard lock(state->statusMutex);
				state->nativeSettings = NativeSettings::Load(nativePath, state->settingsNotice);
				state->nativeSettingsHealthy = state->settingsNotice != "load-failed";
			}
			std::set<fs::path> failedPaths;
			ActiveSettings::SetPaths(false, ExecutablePath(), data, data, paths.cache, data, failedPaths, paths.logs);
			Require(failedPaths.empty(), "Application data is not writable");
			GetConfigHandle().SetFilename((data / "settings.xml").wstring());
			Require(GetConfigHandle().Load(), "Unable to load Cemu settings");
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
			CemuCommonInit(data, false, paths.cache / "title_list_cache.xml", false);
			state->inputReady = true;
			auto graphicPacks = std::make_unique<GraphicPackService>();
			cemuLog_log(LogType::Force, "NativeHost graphic packs: Launcher-managed, new packs disabled");
			Require(fs::equivalent(ActiveSettings::GetMlcPath(), data / "mlc01"), "Loaded MLC escaped application data");
			InitializeGlobalVulkan();
			auto textures = std::make_unique<TextureService>(data, fs::path{}, fs::path{});
			{
				std::lock_guard lock(state->statusMutex);
				state->input = std::make_unique<InputService>(); state->av = std::make_unique<AvSettings>();
				state->accounts = std::make_unique<AccountService>();
				state->textures = std::move(textures);
				state->RebindManagersLocked(installed);
				state->graphicPacks = std::move(graphicPacks);
				state->graphicPackMonitor.SetPaths({data / "graphicPacks"});
				state->graphicPackSynchronizing = true;
				state->corePhase = "ready";
			}
			if (state->launch && (!state->nativeSettingsHealthy || !state->installation->TryBeginPlay())) state->launch = false;
			state->Phase("ready");
			while (!state->stop)
			{
				{
					std::unique_lock lock(state->workMutex);
					state->wake.wait(lock, [&] { return state->stop || state->launch || state->networkTest || state->installationRequested; });
				}
				if (state->installationRequested.exchange(false))
				{
					GameInstallationService::Request request;
					{ std::lock_guard lock(state->workMutex); request = std::move(state->installationRequest); }
					// 走査中の TitleInfo と配置 rename を競合させない。待機は coordinator で行う。
					while (CafeTitleList::IsScanning()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
					if (!state->stop && !state->installationCancelled)
					{
						const auto accepted = state->installation->Start(std::move(request));
						if (state->stop || state->installationCancelled) state->installation->Cancel();
						{
							std::lock_guard lock(state->statusMutex);
							if (accepted) state->installationQueueProgress.reset();
							else
							{
								state->installationQueueProgress->completion = GameInstallationService::Completion::Failed;
								state->installationQueueProgress->result = accepted;
								state->installationQueueProgress->cancellable = false;
							}
						}
						if (accepted) state->installation->Wait();
						const auto current = state->installation->GetSnapshot();
						if (current.state != GameInstallationService::State::RecoveryRequired)
						{
							CafeTitleList::Refresh();
							while (CafeTitleList::IsScanning()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
						}
						std::lock_guard workLock(state->workMutex);
						std::lock_guard lock(state->statusMutex);
						state->RebindManagersLocked(current);
						state->gameInstalled = current.ready;
						state->launchRegionSupported = current.launchSupported;
						++state->gameListRevision;
					}
					{
						std::lock_guard lock(state->statusMutex);
						state->installationBusy = false;
						if (state->installationQueueProgress && state->installationCancelled)
						{
							state->installationQueueProgress->completion = GameInstallationService::Completion::Cancelled;
							state->installationQueueProgress->phase = GameInstallationService::Phase::Completed;
							state->installationQueueProgress->cancellable = false;
							state->installationQueueProgress->result = {GameInstallationService::Error::Cancelled, "Installation cancelled before starting"};
						}
					}
					state->Publish();
					continue;
				}
				if (state->networkTest.exchange(false) && !state->stop)
				{
					state->accounts->RunConnectionTest();
					state->Publish();
					continue;
				}
				if (!state->stop && (!state->gameInstalled || !state->launchRegionSupported || !state->nativeSettingsHealthy))
				{
					state->launch = false;
					state->installation->EndPlay();
					state->Publish();
					continue;
				}
				if (!state->stop)
				{
					if (!state->sessionId)
					{
						++state->sessionId;
						state->presentationSession = state->sessionId.load();
						Require(state->Dispatch([state = state] {
							return state->callbacks.beginPresentation && state->callbacks.beginPresentation(state->sessionId);
						}, true), "Unable to arm black startup screen");
					}
					if (!state->titleStop) state->Phase("preparing");
					try
					{
						if (!state->Dispatch([state = state] { return state->callbacks.prepareCanvas(GetConfig().graphic_api == kOpenGL); }, true))
						{
							Require(state->stop || state->titleStop, "Canvas preparation failed");
						}
						if (!state->stop && !state->titleStop)
						{
							if (!miiAttempted) LogMiiCleanup(MiiInitializationStorage::Recover(state->dataRoot));
							state->accounts->PrepareLaunch();
							// Coreは同一プロセスに残るため、ACTだけ次の初期化で選択Accountを再読込する。
							iosuAct_invalidateAccountCache();
							state->av->PrepareLaunch();
							{ std::lock_guard lock(state->statusMutex); state->error.clear(); }
							NativeSettings settings;
							{ std::lock_guard lock(state->statusMutex); settings = state->nativeSettings; }
							const auto installed = state->installation->GetSnapshot();
							cemuLog_log(LogType::Force, "NativeHost launch snapshot: session={} region={} base={:016x} update={:016x} path={}", state->sessionId.load(), MinecraftTitle::RegionName(installed.baseTitleId), installed.baseTitleId, installed.updateTitleId, _pathToUtf8(installed.basePath));
							Require(installed.launchSupported && MinecraftTitle::IsBase(installed.baseTitleId) && state->managerTitleId == installed.baseTitleId, "Installed Minecraft title is unavailable");
							{ std::lock_guard workLock(state->workMutex); state->graphicPacks->PrepareLaunch(settings.vsyncFrequency, settings.width, settings.height, installed.baseTitleId); }
							auto prepared = GameLaunch::Prepare(installed.basePath);
							cemuLog_log(LogType::Force, "NativeHost GameLaunch::Prepare: result={}", GameLaunch::ErrorName(prepared.error));
							if (prepared.error != GameLaunch::Error::None)
								throw std::runtime_error(fmt::format("Minecraft title preparation failed: {}", GameLaunch::ErrorName(prepared.error)));
							cemuLog_log(LogType::Force, "NativeHost prepared title: id={:016x} version={}", CafeSystem::GetForegroundTitleId(), CafeSystem::GetForegroundTitleVersion());
							Require(CafeSystem::GetForegroundTitleId() == installed.baseTitleId, "Prepared Minecraft BASE title does not match the installed title");
							Require(CafeSystem::GetForegroundTitleVersion() == 560, "Prepared Minecraft title is not update v560");
							Require(CafeTitleList::HasTitleAndVersion(installed.updateTitleId, 560), "Installed Minecraft update v560 is not in the Core title list");
							// sysapp と同じ実効 platform region から Mii Maker の保存先を得る。
							const auto mii = ValidateMinecraftMiiData(ActiveSettings::GetMlcPath(), _SYSGetSystemApplicationTitleId(4), s_enableMiiInitialization && installed.baseTitleId == 0x00050000101dbe00ULL);
							if (mii.NeedsInitialization())
							{
								Require(!miiAttempted, "Mii data initialization may only run once per PLAY");
								miiAttempted = true;
								state->miiInitializing = true;
								state->miiGuestExited = false;
								state->miiReturnStatus = 0;
								miiTimedOut = false;
								miiDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
								state->Phase("mii-initializing");
								const auto rpx = fsc_extractFile((CafeSystem::GetInternalVirtualCodeFolder() + "Minecraft.Client.rpx").c_str());
								Require(bool(rpx), "Mii initialization RPX is unavailable");
								std::array<uint8_t, SHA256_DIGEST_LENGTH> digest{};
								SHA256(rpx->data(), rpx->size(), digest.data());
								std::string hash;
								for (auto byte : digest) hash += fmt::format("{:02x}", byte);
								Require(MiiInitialization::SupportsRpx(hash), "This JPN RPX is not supported for Mii data initialization.");
								std::vector<fs::path> titles{installed.basePath, installed.updatePath};
								if (installed.dlcTitleId) titles.push_back(GameInstallationService::TitlePath(state->dataRoot, installed.dlcTitleId));
								miiStorage = std::make_unique<MiiInitializationStorage>(state->dataRoot, titles, cancelMii);
								miiStorage->PrepareGuestDirectories(ActiveSettings::GetPersistentId());
								Require(!cancelMii(), "Mii initialization cancelled");
								CafeSystem::ShutdownTitle();
								FileStream::SetCreationObserver([storage = miiStorage.get()](const fs::path& path, HANDLE file) { storage->ObserveCreated(path, file); });
								Require(MiiInitialization::Request(hash), "Unable to register Mii initialization descriptor");
								Require(CafeSystem::PrepareMiiInitializationTitle(miiStorage->WorkingMlc()) == CafeSystem::PREPARE_STATUS_CODE::SUCCESS, "Unable to prepare Mii initialization runtime");
								state->miiGeneration = CafeSystem::GetTitleGeneration();
								++state->miiInitializationCount;
								iosuAct_invalidateAccountCache();
							}
							cemuLog_log(LogType::Force, "NativeHost Mii prerequisites verified; foreground title prepared");
							const auto textureFiles = state->miiInitializing ? decltype(state->textures->BeginTitle(0, 0)){} : state->textures->BeginTitle(CafeSystem::GetForegroundTitleId(), CafeSystem::GetForegroundTitleVersion());
							std::vector<std::string> texturePaths;
							for (const auto& file : textureFiles) texturePaths.push_back(file.virtualPath);
							state->graphicPacks->ValidateReplacements(texturePaths);
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
								state->Phase(state->miiInitializing ? "mii-initializing" : useOpenGL ? "opengl" : "vulkan");
								{
									std::lock_guard lock(state->statusMutex);
									state->activeRenderer = useOpenGL ? "OpenGL" : "Vulkan";
								}
								if (useOpenGL)
								{
									const auto session = state->sessionId.load(), binding = ++state->presentationBinding;
									openGL = std::make_unique<NativeOpenGL>(state->callbacks.openGLCanvas(), settings.width, settings.height, [state = state, session, binding] {
										++state->glPresents;
										if (!state->firstFrameVisible) state->FirstFrame(session, binding);
									});
									++state->glContextsCreated;
									g_renderer = std::make_unique<OpenGLRenderer>();
									std::lock_guard lock(state->statusMutex); state->rendererDevice = openGL->DeviceName();
								}
								else
								{
									Require(InitializeGlobalVulkan(), "Vulkan loader is unavailable");
									const auto canvas = state->callbacks.vulkanCanvas();
									Require(IsWindow(canvas), "Vulkan game canvas is unavailable");
									s_windowInfo.canvas_main = {WindowSystem::WindowHandleInfo::Backend::Windows, nullptr, canvas};
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
									state->BindPresentationChain(chain.m_swapchain);
									++state->surfaceCreates;
									++state->swapchainCreates;
									state->swapchainWidth = chain.getExtent().width;
									state->swapchainHeight = chain.getExtent().height;
									InstallVulkanTrace(*state);
									std::lock_guard lock(state->statusMutex); state->rendererDevice = renderer->GetDeviceName();
								}
								g_isGPUInitFinished = false;
								LatteIdleWait::SetEnabled(!useOpenGL);
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
								if (!state->miiInitializing) state->Phase("running");
								std::unique_lock lock(state->workMutex);
								if (state->miiInitializing)
								{
									if (!state->wake.wait_until(lock, miiDeadline, [&] { return state->stop || state->titleStop || state->miiGuestExited; }))
									{ miiTimedOut = true; throw std::runtime_error("Mii initialization timed out"); }
								}
								else state->wake.wait(lock, [&] { return state->stop || state->titleStop; });
							}
						}
					}
					catch (const std::exception& error)
					{
						cemuLog_log(LogType::Force, "NativeHost title launch failed: {}", error.what());
						std::lock_guard lock(state->statusMutex); state->error = error.what();
					}
					if (!state->stop && !state->miiInitializing) state->StopTitle("title-shutdown");
					// coordinator だけが停止・join・Renderer 解放を行う。UI STA は待たない。
					state->presentationSession = 0;
					state->CancelTitleUi();
					if (state->miiInitializing) { StopMiiTitle(); FileStream::SetCreationObserver({}); }
					else CafeSystem::ShutdownTitle();
					LatteIdleWait::SetEnabled(false);
					fscDeviceRedirect_removeOwned(state->textures.get());
					Require(fscDeviceRedirect_ownedCount(state->textures.get()) == 0, "Texture overlay was not released");
					state->textures->EndTitle();
					{ std::lock_guard workLock(state->workMutex); state->graphicPacks->EndTitle(); }
					state->av->EndTitle();
					Require(!CafeSystem::IsTitleRunning() && Latte_GetStopSignal(), "Title threads did not stop");
					ReleaseRenderer();
					MinecraftNativeResolution::End();
					LatteTiming_disableCustomVsyncFrequency();
					state->appliedVsync = 0;
					Require(state->swapchainCreates == state->swapchainDestroys && state->surfaceCreates == state->surfaceDestroys, "Title Vulkan resources were not released");
					// 停止要求や確認表示では解除せず、title/GPU 解放完了を正本にする。
					state->titleRunning = false;
					++state->titleShutdownCount;
					state->fps = 0;
					state->swapchainWidth = state->swapchainHeight = 0;
					cemuLog_log(LogType::Force, "NativeHost title shutdown complete: session={} rendererPresent={} GPUStopped={}", state->sessionId.load(), bool(g_renderer), Latte_GetStopSignal());
					if (state->miiInitializing)
					{
						try
						{
							Require(!state->miiRestartRequired && !cancelMii(), "Mii initialization cancelled, timed out or requires restart");
							const auto result = MiiInitialization::GetResult();
							Require(state->miiGuestExited && state->miiReturnStatus == MiiInitialization::BoundaryExitStatus &&
								result.outcome == MiiInitialization::Outcome::Boundary && result.generation == state->miiGeneration && miiStorage, "Mii initialization boundary was not completed");
							state->Phase("mii-committing");
							miiStorage->Commit(cancelMii);
							LogMiiCleanup(miiStorage->Cleanup(cancelMii));
							Require(!cancelMii(), "Mii initialization cancelled");
							miiStorage.reset();
							Require(state->Dispatch([state = state] { return state->callbacks.resetPresentation && state->callbacks.resetPresentation(state->sessionId); }, true, false), "Cannot retain startup cover");
							{
								std::lock_guard lock(state->workMutex);
								Require(!state->stop && !state->titleStop, "Mii initialization cancelled");
								std::lock_guard uiLock(state->uiMutex);
								state->acceptingTitle = true;
								++state->generation;
								state->presentationSession = state->sessionId.load();
								state->firstFrameQueued = state->firstFrameVisible = false;
								state->miiInitializing = false;
							}
							state->Phase("preparing");
							continue;
						}
						catch (const std::exception& error)
						{
							cemuLog_log(LogType::Force, "Mii initialization stopped without normal launch: {}", error.what());
							std::lock_guard lock(state->statusMutex);
							state->error = state->miiRestartRequired ? "Mii data initialization could not stop. Restart Launcher." : miiTimedOut ? "Mii data initialization timed out. Please try again." :
								state->stop || state->titleStop ? "" : state->error == "This JPN RPX is not supported for Mii data initialization." ? state->error :
								"Mii data initialization failed. Existing data was preserved.";
						}
						state->miiInitializing = false;
						if (miiStorage) LogMiiCleanup(miiStorage->Abandon());
						miiStorage.reset();
					}
					if (!state->stop)
					{
						state->Phase("title-stopped");
						Require(state->Dispatch([state = state] { return state->callbacks.titleStopped(); }, true, false) || state->stop, "Unable to restore Launcher Home");
						{
							std::lock_guard workLock(state->workMutex);
							std::lock_guard statusLock(state->statusMutex);
							state->launch = false;
							miiAttempted = false;
							state->installation->EndPlay();
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
		state->installation->Cancel();
		state->installation->Wait();
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
		state->titleRunning = false;
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

CemuHost::CemuHost(HWND root, HWND canvas, Callbacks callbacks, std::shared_ptr<GameInstallationService> installation, std::shared_ptr<SessionHistory> history) : m_impl(std::make_unique<Impl>())
{
	Require(s_currentHost.expired(), "Only one Core host may exist");
	m_impl->state = std::make_shared<HostState>(root, canvas, std::move(callbacks));
	Require(installation != nullptr, "Game installation service is required");
	m_impl->state->installation = std::move(installation);
	m_impl->state->history = std::move(history);
	s_currentHost = m_impl->state;
	swkbd::host::Enable(true);
	LatteOverlay_setShaderProgressVisible(false);
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
	swkbd::host::Enable(false);
	LatteOverlay_setShaderProgressVisible(true);
}
void CemuHost::Start(bool launchImmediately)
{
	Require(!m_impl->worker.joinable(), "Core initialization may run only once");
	m_impl->state->launch = launchImmediately;
	m_impl->state->stopped = false;
	try { m_impl->worker = std::thread([impl = m_impl.get()] { impl->Run(); }); }
	catch (...) { m_impl->state->stopped = true; throw; }
}
bool CemuHost::TryBeginLauncherUpdate()
{
	std::lock_guard workLock(m_impl->state->workMutex);
	std::lock_guard statusLock(m_impl->state->statusMutex);
	const auto& state = *m_impl->state;
	const LauncherUpdate::Readiness readiness{
		.titleStopped = !state.launch && !state.titleStop && !state.confirmationId && (state.titlePhase == "idle" || state.titlePhase == "stopped"),
		.coreSafe = state.corePhase == "ready" && state.phase == "ready",
		.rendererStopped = !state.presentationSession && state.swapchainCreates == state.swapchainDestroys && state.surfaceCreates == state.surfaceDestroys && state.glContextsCreated == state.glContextsDestroyed,
		.historySaved = !state.history || state.history->IsDurable(),
		.settingsSaved = state.CanEditSettingsLocked(),
		.canClose = !state.stop && !state.stopped && !state.failed
	};
	if (!readiness.CanBegin()) return false;
	m_impl->state->launcherUpdate = true;
	return true;
}
void CemuHost::CancelLauncherUpdate()
{
	std::lock_guard lock(m_impl->state->statusMutex);
	m_impl->state->launcherUpdate = false;
}
void CemuHost::LaunchGame()
{
	{
		std::lock_guard workLock(m_impl->state->workMutex);
		std::lock_guard statusLock(m_impl->state->statusMutex);
		m_impl->state->graphicPackMonitor.Poll(false);
		if (m_impl->state->graphicPackMonitor.HasPending() || m_impl->state->graphicPackReloadPending)
		{
			m_impl->state->graphicPackSynchronizing = true;
			return;
		}
		if (!m_impl->state->gameInstalled || !m_impl->state->launchRegionSupported || !m_impl->state->nativeSettingsHealthy || !m_impl->state->CanEditSettingsLocked()) return;
		if (!m_impl->state->installation->TryBeginPlay()) return;
		if (m_impl->state->input) m_impl->state->input->CancelCapture();
		m_impl->state->launch = true;
		m_impl->state->titleStop = false;
		m_impl->state->ClearStopConfirmationLocked();
		++m_impl->state->sessionId;
		m_impl->state->titlePhase = "preparing";
		m_impl->state->stopReason.clear();
		{ std::lock_guard lock(m_impl->state->uiMutex); m_impl->state->acceptingTitle = true; ++m_impl->state->generation; }
		m_impl->state->phase = "preparing";
		m_impl->state->presentationSession = m_impl->state->sessionId.load();
		m_impl->state->firstFrameQueued = false;
		m_impl->state->firstFrameVisible = false;
		m_impl->state->freshFrameRequired = false;
	}
	const auto session = m_impl->state->sessionId.load();
	if (!m_impl->state->Dispatch([state = m_impl->state, session] {
		return state->CanPresent(session) && state->callbacks.beginPresentation && state->callbacks.beginPresentation(session);
	}, true)) m_impl->state->Fail("Unable to arm black startup screen");
	if (m_impl->state->logReady) cemuLog_log(LogType::Force, "NativeHost Black Screen Armed: session={}", session);
	m_impl->state->Publish();
	m_impl->state->wake.notify_all();
}
void CemuHost::StopGame() { m_impl->state->StopTitle("emulator-stop"); }
std::string CemuHost::ErrorMessage() const
{
	std::lock_guard lock(m_impl->state->statusMutex);
	return m_impl->state->error;
}
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
GameInstallationService::Result CemuHost::StartGameDataOperation(GameInstallationService::Request request)
{
	auto& state = *m_impl->state;
	{
		std::lock_guard workLock(state.workMutex);
		std::lock_guard statusLock(state.statusMutex);
		if (!state.nativeSettingsHealthy || !state.CanEditSettingsLocked() || state.installation->GetSnapshot().state == GameInstallationService::State::RecoveryRequired) return {GameInstallationService::Error::Busy, "Stop the game and other management operations first; check native settings and recovery status"};
		state.installationQueueProgress = GameInstallationService::Progress{request.operation, GameInstallationService::Phase::Analyzing, 0, 0, 0, 0, true, GameInstallationService::Completion::Running, {}};
		state.installationRequest = std::move(request);
		state.installationCancelled = false;
		state.installationBusy = true;
		state.installationRequested = true;
	}
	state.Publish();
	state.wake.notify_all();
	return {};
}
GameInstallationService::Snapshot CemuHost::GameDataStatus() const
{
	std::lock_guard lock(m_impl->state->statusMutex);
	return m_impl->state->InstallationStatusLocked();
}
bool CemuHost::CanManageGameData() const
{
	std::lock_guard lock(m_impl->state->statusMutex);
	const auto installed = m_impl->state->InstallationStatusLocked();
	return m_impl->state->nativeSettingsHealthy && m_impl->state->CanEditSettingsLocked() &&
		installed.state != GameInstallationService::State::RecoveryRequired && installed.state != GameInstallationService::State::Error;
}
std::future<GameInstallationService::Analysis> CemuHost::AnalyzeGameSource(std::filesystem::path selected) const
{
	return std::async(std::launch::async, [installation = m_impl->state->installation, selected = std::move(selected)] { return installation->AnalyzeSource(selected); });
}
void CemuHost::CancelGameDataOperation()
{
	auto& state = *m_impl->state;
	std::lock_guard workLock(state.workMutex);
	std::lock_guard statusLock(state.statusMutex);
	state.installationCancelled = true;
	state.installation->Cancel();
}
void CemuHost::RequestStop() { m_impl->state->installation->Cancel(); m_impl->state->Stop(); }
void CemuHost::ReportDisplayFailure() { m_impl->state->Fail("Unable to switch launcher display mode"); }
bool CemuHost::IsStopped() const { return m_impl->state->stopped; }
bool CemuHost::HasRunningTitle() const { return m_impl->state->titleRunning; }
bool CemuHost::IsRunning() const
{
	std::lock_guard lock(m_impl->state->statusMutex);
	return m_impl->state->phase == "running" && !m_impl->state->stop;
}
bool CemuHost::IsTitleStopping() const { return m_impl->state->titleStop && m_impl->state->launch; }
bool CemuHost::IsTitleActive() const
{
	return m_impl->state->launch && m_impl->state->presentationSession == m_impl->state->sessionId &&
		m_impl->state->sessionId && !m_impl->state->titleStop && !m_impl->state->stop;
}
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
		if (!state.nativeSettingsHealthy) state.settingsNotice = "load-failed";
		else if (!state.CanEditSettingsLocked()) state.settingsNotice = "not-ready";
		else if (!valid) state.settingsNotice = "invalid";
		else
		{
			const auto path = state.dataRoot / "launcher-native-settings.json";
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

std::string CemuHost::SynchronizeUi(bool full)
{
	auto& state = *m_impl->state;
	std::lock_guard workLock(state.workMutex);
	std::lock_guard statusLock(state.statusMutex);
	if (state.corePhase != "ready" || state.stop || state.failed || !state.accounts) return "{}";
	if (!state.friendsStarted && !state.installationBusy && (state.phase == "ready" || state.phase == "running"))
	{
		if (state.phase != "running") state.accounts->PrepareLaunch();
		FriendsSession::Instance().ConfigureCurrent(true, state.accounts->NetworkName());
		state.friendsStarted = true;
	}
	state.graphicPackMonitor.SetPaths({state.dataRoot / "graphicPacks"});
	state.textureMonitor.SetPaths({state.dataRoot / "Textures"});
	state.worldMonitor.SetPaths(state.worlds ? state.worlds->WatchPaths(ActiveSettings::GetPersistentId()) : std::vector<fs::path>{});
	const bool idle = state.phase == "ready" && !state.launch && !state.titleStop && !state.confirmationId && !state.installationBusy && !state.textureBusy && !state.graphicPackBusy && !state.openingWorldFolder;
	if (state.graphicPackMonitor.Poll(idle)) state.graphicPackReloadPending = true;
	if (state.graphicPackReloadPending && idle && state.graphicPacks && state.graphicPacks->Reload())
	{
		state.graphicPackReloadPending = false; ++state.graphicPackRevision;
	}
	if (state.textureMonitor.Poll(!state.installationBusy && !state.textureBusy)) ++state.textureRevision;
	if (state.worldMonitor.Poll(state.CanManageWorldsLocked() && state.RegionManagersAvailable())) ++state.worldRevision;
	const bool synchronizing = state.graphicPackMonitor.HasPending() || state.graphicPackReloadPending;
	if (synchronizing != state.graphicPackSynchronizing)
	{
		state.graphicPackSynchronizing = synchronizing;
		state.callbacks.post([shared = m_impl->state] { if (shared->alive && !shared->stop) shared->Publish(); });
	}
	rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	writer.StartObject();
	writer.Key("graphicPacks"); writer.Uint64(state.graphicPackRevision);
	writer.Key("texture"); writer.Uint64(state.textureRevision);
	writer.Key("world"); writer.Uint64(state.worldRevision);
	writer.Key("graphicPacksDeferred"); writer.Bool(synchronizing);
	writer.Key("monitorAvailable"); writer.Bool(state.graphicPackMonitor.IsAvailable() && state.textureMonitor.IsAvailable() && state.worldMonitor.IsAvailable());
	const auto friendsVersion = FriendsSession::Instance().Version();
	if (full || friendsVersion != state.friendsVersion)
	{
		state.friendsVersion = friendsVersion;
		writer.Key("friends"); const auto friends = FriendsSnapshotJson(); writer.RawValue(friends.data(), friends.size(), rapidjson::kObjectType);
	}
	writer.EndObject(); return {buffer.GetString(), buffer.GetSize()};
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
		// Shell の COM 呼出し中は Core のロックを外し、競合操作は openingWorldFolder で止める。
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
	if (!state.accounts || state.stop || state.failed || state.openingWorldFolder) return "{\"notice\":\"not-ready\"}";
	if (!state.worlds) return "{\"notice\":\"installation-region-required\",\"activeId\":0,\"revision\":0,\"canEdit\":false,\"recoveryRequired\":false,\"worlds\":[],\"accounts\":[],\"trash\":[],\"trashBytes\":0}";
	if (!requestId || requestId <= state.lastWorldRequest) return "{\"notice\":\"stale-request\"}";
	state.lastWorldRequest = requestId;
	return state.worlds->Command(payload, state.WorldContext());
}

void CemuHost::TextureCommand(uint64_t requestId, std::string payload, std::function<void(std::string)> complete)
{
	auto state = m_impl->state;
	std::lock_guard workLock(state->workMutex);
	std::lock_guard statusLock(state->statusMutex);
	if (!state->textures || state->stop || state->failed || state->installationBusy || state->textureBusy || !requestId || requestId <= state->lastTextureRequest)
	{
		complete("{\"notice\":\"not-ready\"}"); return;
	}
	const bool editable = state->RegionManagersAvailable() && state->CanEditWorldsLocked() && (!state->worlds || state->worlds->IsHealthy());
	const bool acknowledgeable = state->CanAcknowledgeTexturesLocked();
	const auto managerGeneration = state->managerGeneration.load();
	state->lastTextureRequest = requestId;
	if (m_impl->textureWorker.joinable()) m_impl->textureWorker.join();
	state->textureBusy = true;
	try
	{
		m_impl->textureWorker = std::thread([state, payload = std::move(payload), complete = std::move(complete), editable, acknowledgeable, managerGeneration] {
			const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			std::string result = "{\"notice\":\"not-ready\"}";
			if (SUCCEEDED(apartment))
			{
				result = state->textures->Command(payload, editable, acknowledgeable);
				CoUninitialize();
			}
			{
				std::lock_guard workLock(state->workMutex); std::lock_guard statusLock(state->statusMutex); state->textureBusy = false;
			}
			state->callbacks.post([state, complete, result = std::move(result), managerGeneration] {
				if (state->alive && !state->stop) { complete(managerGeneration == state->managerGeneration ? result : "{\"notice\":\"not-ready\"}"); state->Publish(); }
			});
		});
	}
	catch (...) { state->textureBusy = false; throw; }
}

std::string CemuHost::OpenTextureFolder(uint64_t requestId, const std::function<bool(const std::wstring&)>& open)
{
	auto& state = *m_impl->state;
	std::unique_lock workLock(state.workMutex); std::unique_lock statusLock(state.statusMutex);
	if (!state.textures || state.stop || state.failed || state.installationBusy || state.textureBusy || GetCurrentThreadId() != state.uiThread || !requestId || requestId <= state.lastTextureRequest) return "{\"notice\":\"not-ready\"}";
	state.lastTextureRequest = requestId;
	if (std::chrono::steady_clock::now() < state.nextTextureFolderOpen) return "{\"notice\":\"folder-open-busy\"}";
	state.nextTextureFolderOpen = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	const bool acknowledgeable = state.CanAcknowledgeTexturesLocked();
	state.textureBusy = true; statusLock.unlock(); workLock.unlock();
	const auto result = state.textures->OpenFolder([&](const fs::path& path) { return open(path.native()); }, acknowledgeable);
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
			profile.load_file((state.dataRoot / "controllerProfiles/controller0.xml").c_str());
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
	const auto output = s_windowInfo.hostOutputState.load();
	if (bool(output & 1) != visible)
	{
		// UI STA だけが可視世代を更新する。入力 gate・Guest 実行状態とは独立する。
		if (!visible)
		{
			if (m_impl->state->firstFrameVisible) m_impl->state->freshFrameRequired = true;
			m_impl->state->firstFrameVisible = false;
			m_impl->state->firstFrameQueued = false;
		}
		s_windowInfo.hostOutputState = output + 1;
	}
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
		if (swkbd::host::Read().active) { s_windowInfo.set_keystatesup(); return; }
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
	bool IsKeyDown(uint32 key) { return !swkbd::host::Read().active && s_windowInfo.get_keystate(key); }
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
					boost::nowide::widen(fmt::format("MCWiiU Launcher - FPS: {:.2f} [{}] [TitleId: {:08X}-{:08X}] [{} v{}]", fps, state->activeRenderer, uint32_t(state->managerTitleId >> 32), uint32_t(state->managerTitleId), MinecraftTitle::RegionName(state->managerTitleId), CafeSystem::GetForegroundTitleVersion()));
				SetWindowTextW(state->root, title.c_str());
				state->callbacks.status(state->Json());
				return true;
			}, false);
		}
	}
	void ShowErrorDialog(std::string_view message, std::string_view title, std::optional<ErrorCategory> category)
	{
		if (auto state = s_currentHost.lock())
		{
			if (category == ErrorCategory::GRAPHIC_PACKS)
			{
				{ std::lock_guard lock(state->statusMutex); state->error = fmt::format("{}: {}", title, message); }
				state->StopTitle("graphic-pack-error");
			}
			else state->Fail(fmt::format("{}: {}", title, message));
		}
	}
	void CaptureInput(const ControllerState& current, const ControllerState& last)
	{
		if (auto state = s_currentHost.lock(); state && s_windowInfo.app_active && state->controllerModifier >= 0 && state->controllerExit >= 0 &&
			current.buttons.GetButtonState(state->controllerModifier) && current.buttons.GetButtonState(state->controllerExit) && !last.buttons.GetButtonState(state->controllerExit))
			state->Stop();
	}
} // namespace WindowSystem

std::string CemuHost::GraphicPackCommand(uint64_t requestId, std::string_view payload)
{
	auto& state = *m_impl->state;
	std::lock_guard workLock(state.workMutex); std::lock_guard statusLock(state.statusMutex);
	if (!state.graphicPacks || !requestId || requestId <= state.lastGraphicPackRequest || state.graphicPackBusy) return "{\"notice\":\"stale-request\"}";
	state.lastGraphicPackRequest = requestId;
	return state.graphicPacks->Command(payload, state.CanEditSettingsLocked());
}

std::string CemuHost::OpenGraphicPackFolder(uint64_t requestId, const std::function<bool(const std::wstring&)>& open)
{
	auto& state = *m_impl->state;
	std::unique_lock workLock(state.workMutex); std::unique_lock statusLock(state.statusMutex);
	if (!state.graphicPacks || !requestId || requestId <= state.lastGraphicPackRequest || state.graphicPackBusy || state.stop || state.failed || GetCurrentThreadId() != state.uiThread) return "{\"notice\":\"not-ready\"}";
	state.lastGraphicPackRequest = requestId;
	if (std::chrono::steady_clock::now() < state.nextGraphicPackFolderOpen) return "{\"notice\":\"folder-open-busy\"}";
	state.nextGraphicPackFolderOpen = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	state.graphicPackBusy = true; statusLock.unlock(); workLock.unlock();
	const auto result = state.graphicPacks->OpenFolder([&](const fs::path& path) { return open(path.native()); });
	workLock.lock(); statusLock.lock(); state.graphicPackBusy = false; return result;
}
