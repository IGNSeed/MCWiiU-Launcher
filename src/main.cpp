#include "WindowSystem.h"
#include "util/crypto/aes128.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "config/CemuConfig.h"
#include "config/NetworkSettings.h"
#include "config/LaunchSettings.h"
#include "input/InputManager.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/TitleList/SaveList.h"

#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "Common/cpu_features.h"

#include "util/helpers/helpers.h"
#include "config/ActiveSettings.h"

#include "Cafe/IOSU/legacy/iosu_crypto.h"
#include "Cafe/OS/libs/vpad/vpad.h"

#include "audio/IAudioAPI.h"
#include "audio/IAudioInputAPI.h"
#if BOOST_OS_WINDOWS
#pragma comment(lib,"Dbghelp.lib")
#endif

#ifdef HAS_SDL
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#endif

#if BOOST_OS_LINUX
#define _putenv(__s) putenv((char*)(__s))
#include <sys/sysinfo.h>
#elif BOOST_OS_MACOS || BOOST_OS_BSD
#define _putenv(__s) putenv((char*)(__s))
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#if BOOST_OS_WINDOWS
extern "C"
{
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
	__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
}
#endif

std::atomic_bool g_isGPUInitFinished = false;

void mainEmulatorLLE();
void ppcAsmTest();
void gx2CopySurfaceTest();
void ExpressionParser_test();
void FSTVolumeTest();
void CRCTest();

void UnitTests()
{
	ExpressionParser_test();
	gx2CopySurfaceTest();
	ppcAsmTest();
	FSTVolumeTest();
	CRCTest();
}

bool isConsoleConnected = false;
void requireConsole()
{
    #if BOOST_OS_WINDOWS
    if (isConsoleConnected)
        return;

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwFileType = GetFileType(hOut);

    if (dwFileType == FILE_TYPE_UNKNOWN || dwFileType == FILE_TYPE_CHAR)
    {
        if (AttachConsole(ATTACH_PARENT_PROCESS) != FALSE)
        {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
            freopen("CONIN$", "r", stdin);
            isConsoleConnected = true;
        }
    }
    else
    {
        isConsoleConnected = true; 
    }
    #endif
}

void HandlePostUpdate()
{
	auto exeBackupPath = ActiveSettings::GetExecutablePath();
	exeBackupPath.replace_extension( _utf8ToPath(_pathToUtf8(exeBackupPath.extension()).append(".backup")));
	std::error_code ec;
	if (!fs::exists(exeBackupPath, ec))
		return;
	// try to delete update residue, but give up quickly as to not cause a permanent hang
	// it may succeed on next turn
	for (sint32 i=0; i<3; i++)
	{
		if (fs::remove(exeBackupPath, ec))
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}
}

void ToolShaderCacheMerger();

#if BOOST_OS_WINDOWS

// entrypoint for release builds
int wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd)
{
	if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE)))
		cemuLog_log(LogType::Force, "CoInitializeEx() failed");
#ifdef HAS_SDL
	SDL_SetMainReady();
#endif
	auto parse_rc = LaunchSettings::HandleCommandline(lpCmdLine);
	if (parse_rc.has_value())
		return *parse_rc;
	WindowSystem::Create();
	return 0;
}

// entrypoint for debug builds with console
int main(int argc, char* argv[])
{
	if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE)))
		cemuLog_log(LogType::Force, "CoInitializeEx() failed");
#ifdef HAS_SDL
	SDL_SetMainReady();
#endif
	auto parse_rc = LaunchSettings::HandleCommandline(argc, argv);
	if (parse_rc.has_value())
		return *parse_rc;
	WindowSystem::Create();
	return 0;
}

#else

int BreathOfTheWildChildProcessMain();
int main(int argc, char *argv[])
{
#if BOOST_OS_LINUX && defined(ENABLE_VULKAN)
	if (getenv("CEMU_DETECT_RADV") != nullptr)
		return BreathOfTheWildChildProcessMain();
#endif

#if BOOST_OS_LINUX || BOOST_OS_BSD
    XInitThreads();
#endif
	auto parse_rc = LaunchSettings::HandleCommandline(argc, argv);
  if (parse_rc.has_value())
		return *parse_rc;
	WindowSystem::Create();
	return 0;
}
#endif

extern "C" DLLEXPORT uint64 gameMeta_getTitleId()
{
	return CafeSystem::GetForegroundTitleId();
}
