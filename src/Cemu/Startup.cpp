#include "Cemu/Startup.h"

#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "util/crypto/aes128.h"
#include "util/helpers/helpers.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "config/NetworkSettings.h"
#include "input/InputManager.h"
#include "audio/IAudioAPI.h"
#include "audio/IAudioInputAPI.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/TitleList/SaveList.h"

#if BOOST_OS_LINUX || BOOST_OS_MACOS || BOOST_OS_BSD
#define _putenv(__s) putenv((char*)(__s))
#endif

std::wstring executablePath;

// some implementations of _putenv dont copy the string and instead only store a pointer
// thus we use a helper to keep a permanent copy
std::vector<std::string*> sPutEnvMap;

void _putenvSafe(const char* c)
{
    auto s = new std::string(c);
    sPutEnvMap.emplace_back(s);
    _putenv(s->c_str());
}

void reconfigureGLDrivers()
{
#ifdef ENABLE_OPENGL
	// reconfigure GL drivers to store
	const fs::path nvCacheDir = ActiveSettings::GetCachePath("shaderCache/driver/nvidia/");

	std::error_code err;
	fs::create_directories(nvCacheDir, err);

	std::string nvCacheDirEnvOption("__GL_SHADER_DISK_CACHE_PATH=");
	nvCacheDirEnvOption.append(_pathToUtf8(nvCacheDir));

#if BOOST_OS_WINDOWS
	std::wstring tmpW = boost::nowide::widen(nvCacheDirEnvOption);
	_wputenv(tmpW.c_str());
#else
    _putenvSafe(nvCacheDirEnvOption.c_str());
#endif
    _putenvSafe("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1");
#endif
}

void reconfigureVkDrivers()
{
#ifdef ENABLE_VULKAN
    _putenvSafe("DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1");
    _putenvSafe("DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1");
#endif
}

void WindowsInitCwd(const fs::path& workingDirectory)
{
	#if BOOST_OS_WINDOWS
	executablePath.resize(4096);
	int i = GetModuleFileNameW(NULL, executablePath.data(), executablePath.size());
	if(i >= 0)
		executablePath.resize(i);
	else
		executablePath.clear();
	// Launcher の EXE とデータが別配置でも、明示した runtime から参照する。
	if (workingDirectory.empty())
		SetCurrentDirectoryW(fs::path(executablePath).parent_path().c_str());
	else if (!SetCurrentDirectoryW(workingDirectory.c_str()))
		throw std::runtime_error("Unable to select the emulator working directory");
	// set high priority
	SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
	#endif
}

void CemuCommonInit(const fs::path& workingDirectory, bool loadGraphicPacks)
{
	reconfigureGLDrivers();
	reconfigureVkDrivers();
	// crypto init
	AES128_init();
	// init PPC timer
	// call this as early as possible because it measures frequency of RDTSC using an asynchronous thread over 3 seconds
	PPCTimer_init();

	WindowsInitCwd(workingDirectory);
    ExceptionHandler_Init();
	// read config
	GetConfigHandle().Load();
	if (NetworkConfig::XMLExists())
		n_config.Load();
	// parallelize expensive init code
	std::future<int> futureInitAudioAPI = std::async(std::launch::async, []{ IAudioAPI::InitializeStatic(); IAudioInputAPI::InitializeStatic(); return 0; });
	std::future<int> futureInitGraphicPacks = std::async(std::launch::async, [loadGraphicPacks]{ if (loadGraphicPacks) GraphicPack2::LoadAll(); return 0; });
	InputManager::instance().load();
	futureInitAudioAPI.wait();
	futureInitGraphicPacks.wait();
	// init Cafe system
	CafeSystem::Initialize();
	// init title list
	CafeTitleList::Initialize(ActiveSettings::GetUserDataPath("title_list_cache.xml"));
	for (auto& it : GetConfig().game_paths)
		CafeTitleList::AddScanPath(_utf8ToPath(it));
	fs::path mlcPath = ActiveSettings::GetMlcPath();
	if (!mlcPath.empty())
		CafeTitleList::SetMLCPath(mlcPath);
	CafeTitleList::Refresh();
	// init save list
	CafeSaveList::Initialize();
	if (!mlcPath.empty())
	{
		CafeSaveList::SetMLCPath(mlcPath);
		CafeSaveList::Refresh();
	}
}
