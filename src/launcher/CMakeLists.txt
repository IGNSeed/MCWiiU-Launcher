cmake_minimum_required(VERSION 3.25)
include("${CMAKE_CURRENT_LIST_DIR}/LauncherVersion.cmake")
project(MCWiiULauncher VERSION ${MCWIIU_LAUNCHER_VERSION} LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "LauncherShell requires Windows x64 with MSVC")
endif()

enable_language(RC)
option(MCWIIU_DEVELOPER_DIAGNOSTICS "Enable launcher developer diagnostic commands" OFF)
option(MCWIIU_JPN_MII_INITIALIZATION "Enable verified JPN first-run Mii initialization" ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(TARGET CemuCafe)
    # 既存 Core と vcpkg static の CRT を変えず、Launcher 側を合わせる。
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set(LAUNCHER_DEPENDENCY_CACHE "${CMAKE_BINARY_DIR}/_dependencies" CACHE PATH "Launcher dependency cache")

include("${CMAKE_CURRENT_LIST_DIR}/DiscordSocialSdk.cmake")

include(FetchContent)
set(saucer_backend WebView2 CACHE STRING "" FORCE)
set(saucer_serializer Glaze CACHE STRING "" FORCE)
set(saucer_examples OFF CACHE BOOL "" FORCE)
set(saucer_tests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(saucer
    URL https://codeload.github.com/saucer/saucer/tar.gz/68345fa45a7465fe5a62b699b51847b916c8c16a
    URL_HASH SHA256=934faaafd3d3fa40508f6010165627942781c5bcf73564fd7a4ea162e7b6db41
    DOWNLOAD_DIR "${LAUNCHER_DEPENDENCY_CACHE}/downloads"
    DOWNLOAD_NAME saucer-8.0.5.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(saucer)
if(TARGET CemuCafe)
    # Core の lean Windows 定義は saucer の GDI+/COM header 前提に適用しない。
    target_compile_options(saucer PRIVATE /UWIN32_LEAN_AND_MEAN)
endif()

add_library(LauncherStorage STATIC LauncherPaths.cpp LauncherPaths.h SingleInstance.cpp SingleInstance.h MiiDatabase.cpp MiiDatabase.h MiiInitializationStorage.cpp MiiInitializationStorage.h)
target_compile_features(LauncherStorage PRIVATE cxx_std_20)
target_compile_options(LauncherStorage PRIVATE /utf-8 /W4)
target_compile_definitions(LauncherStorage PRIVATE NOMINMAX)
target_link_libraries(LauncherStorage PUBLIC Shell32 Ole32 Advapi32 Comctl32 Bcrypt)

add_library(LauncherHistory STATIC SessionHistory.cpp SessionHistory.h)
target_compile_features(LauncherHistory PRIVATE cxx_std_23)
target_compile_options(LauncherHistory PRIVATE /utf-8 /W4)
target_compile_definitions(LauncherHistory PRIVATE NOMINMAX)
target_link_libraries(LauncherHistory PUBLIC LauncherStorage PRIVATE glaze::glaze mincore)

add_library(LauncherInstallation STATIC GameInstallationService.cpp GameInstallationService.h)
target_compile_features(LauncherInstallation PRIVATE cxx_std_20)
target_compile_options(LauncherInstallation PRIVATE /utf-8 /W4)
target_compile_definitions(LauncherInstallation PRIVATE NOMINMAX)
target_link_libraries(LauncherInstallation PUBLIC LauncherStorage PRIVATE XmlLite Shlwapi Bcrypt)

if(NOT TARGET OpenSSL::Crypto)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
    set(OPENSSL_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../out/build/Release/vcpkg_installed/x64-windows-static" CACHE PATH "Existing OpenSSL dependency")
    find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
endif()
if(NOT TARGET libzip::zip)
    list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_LIST_DIR}/../../out/build/Release/vcpkg_installed/x64-windows-static")
    find_package(libzip CONFIG REQUIRED)
endif()
add_library(LauncherUpdate STATIC UpdateSecurity.cpp UpdateSecurity.h UpdateService.cpp UpdateService.h UpdateHandoff.cpp UpdateHandoff.h UpdateIdentity.h VersionedInstallation.cpp VersionedInstallation.h)
target_compile_features(LauncherUpdate PRIVATE cxx_std_23)
target_compile_options(LauncherUpdate PRIVATE /utf-8 /W4)
target_compile_definitions(LauncherUpdate PRIVATE NOMINMAX LAUNCHER_VERSION="${PROJECT_VERSION}")
target_compile_definitions(LauncherUpdate PUBLIC MCWIIU_FOUNDATION_VERSION="${MCWIIU_FOUNDATION_VERSION}")
target_link_libraries(LauncherUpdate PUBLIC LauncherStorage PRIVATE OpenSSL::Crypto libzip::zip glaze::glaze Winhttp User32 Ws2_32 Crypt32 Version Wintrust)
add_executable(MCWiiU-SigningTool EXCLUDE_FROM_ALL installer/SigningTool.cpp)
target_compile_options(MCWiiU-SigningTool PRIVATE /utf-8 /W4)
target_compile_definitions(MCWiiU-SigningTool PRIVATE NOMINMAX)
target_link_libraries(MCWiiU-SigningTool PRIVATE LauncherUpdate OpenSSL::Crypto libzip::zip glaze::glaze)
set_target_properties(MCWiiU-SigningTool PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_executable(MCWiiU-Updater WIN32 updater/main.cpp LauncherShell.manifest)
target_link_libraries(MCWiiU-Updater PRIVATE LauncherUpdate)
target_compile_options(MCWiiU-Updater PRIVATE /utf-8 /W4)
target_compile_definitions(MCWiiU-Updater PRIVATE NOMINMAX)
add_executable(MCWiiU-Launcher WIN32 bootstrapper/main.cpp LauncherShell.manifest)
target_link_libraries(MCWiiU-Launcher PRIVATE LauncherUpdate)
target_compile_options(MCWiiU-Launcher PRIVATE /utf-8 /W4)
target_compile_definitions(MCWiiU-Launcher PRIVATE NOMINMAX)
add_library(MCWiiU-SetupGuard SHARED installer/SetupGuard.cpp)
target_compile_options(MCWiiU-SetupGuard PRIVATE /utf-8 /W4)
target_compile_definitions(MCWiiU-SetupGuard PRIVATE NOMINMAX)
target_link_libraries(MCWiiU-SetupGuard PRIVATE LauncherUpdate Msi)
set_target_properties(MCWiiU-Updater MCWiiU-Launcher MCWiiU-SetupGuard PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_executable(LauncherShell WIN32 GameDataBridge.cpp GameDataBridge.h main.cpp NativeExitDialog.cpp NativeExitDialog.h NativeGameHost.cpp NativeGameHost.h LauncherPreferences.cpp LauncherPreferences.h LauncherShell.manifest)
mcwiiu_configure_discord_presence()
configure_file(LauncherVersion.rc.in "${CMAKE_CURRENT_BINARY_DIR}/LauncherVersion.rc" @ONLY)
configure_file(FoundationVersion.rc.in "${CMAKE_CURRENT_BINARY_DIR}/FoundationVersion.rc" @ONLY)
target_sources(LauncherShell PRIVATE LauncherIcon.rc LauncherIcon.h "${CMAKE_CURRENT_BINARY_DIR}/LauncherVersion.rc")
target_sources(MCWiiU-Updater PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/FoundationVersion.rc")
target_sources(MCWiiU-Launcher PRIVATE LauncherIcon.rc "${CMAKE_CURRENT_BINARY_DIR}/FoundationVersion.rc")
target_sources(MCWiiU-SetupGuard PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/FoundationVersion.rc")
target_include_directories(LauncherShell PRIVATE "$<$<COMPILE_LANGUAGE:RC>:${CMAKE_CURRENT_SOURCE_DIR}>")
set_property(SOURCE LauncherIcon.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/LauncherIcon.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/app-icon.ico")
target_compile_features(LauncherShell PRIVATE cxx_std_23)
target_compile_options(LauncherShell PRIVATE /utf-8 /W4)
target_compile_definitions(LauncherShell PRIVATE NOMINMAX LAUNCHER_VERSION="${PROJECT_VERSION}")
if(MCWIIU_DEVELOPER_DIAGNOSTICS)
    target_compile_definitions(LauncherShell PRIVATE MCWIIU_DEVELOPER_DIAGNOSTICS)
endif()
# stable native API が参照する既存 WebView2 SDK も引き継ぐ。
saucer_add_module(LauncherShell)
add_dependencies(LauncherShell MCWiiU-Updater MCWiiU-Launcher MCWiiU-SetupGuard)
target_link_libraries(LauncherShell PRIVATE LauncherUpdate LauncherStorage LauncherInstallation LauncherHistory Comctl32 glaze::glaze)
set_target_properties(LauncherShell PROPERTIES OUTPUT_NAME "MCWiiU Launcher" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(target IN ITEMS LauncherShell MCWiiU-Launcher MCWiiU-Updater MCWiiU-SetupGuard MCWiiU-SigningTool)
    target_link_options(${target} PRIVATE /PDBALTPATH:MCWiiU-Launcher.pdb)
endforeach()
saucer_embed("frontend" TARGET LauncherShell DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/embedded")
# 埋め込み配列のサイズ変更時にも、宣言を参照する main を再コンパイルする。
set_property(SOURCE main.cpp APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/app.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/i18n.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/locales.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/game-data.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/friends.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/world.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/texture.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/graphic-packs.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/motion.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/session-history.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/updates.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/index.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/style.css"
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/assets/minecraft-wiiu-home.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/assets/pack_icon.png")
target_link_libraries(LauncherShell PRIVATE saucer::embedded)

if(TARGET CemuCafe)
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/bin/launcher-package.json" CONTENT "{\"native_core\":true,\"version\":\"${PROJECT_VERSION}\",\"developer_diagnostics\":$<IF:$<BOOL:${MCWIIU_DEVELOPER_DIAGNOSTICS}>,true,false>}\n")
    target_compile_options(LauncherShell PRIVATE /UWIN32_LEAN_AND_MEAN)
    target_sources(LauncherShell PRIVATE LauncherResources.rc)
    target_include_directories(LauncherShell PRIVATE "$<$<COMPILE_LANGUAGE:RC>:${CMAKE_CURRENT_SOURCE_DIR}/..>")
    add_library(CemuNativeHost STATIC CemuHost.cpp CemuHost.h WorldService.cpp WorldService.h WorldImport.cpp WorldImport.h WorldMetadata.cpp WorldMetadata.h FriendsService.cpp FriendsService.h AccountService.cpp AccountService.h InputService.cpp InputService.h AvSettings.cpp AvSettings.h NativeOpenGL.cpp NativeOpenGL.h NativeSettings.cpp NativeSettings.h ../Cemu/Startup.cpp ../Cemu/GameLaunch.cpp)
    set_target_properties(CemuNativeHost PROPERTIES CXX_STANDARD 20 CEMU_NATIVE_HOST TRUE)
    target_compile_definitions(CemuNativeHost PRIVATE $<$<BOOL:${MCWIIU_JPN_MII_INITIALIZATION}>:MCWIIU_JPN_MII_INITIALIZATION>)
    target_sources(CemuNativeHost PRIVATE GraphicPackService.cpp GraphicPackService.h TextureService.cpp TextureService.h TextureCompatibility.cpp TextureCompatibility.h DirectoryChangeMonitor.cpp DirectoryChangeMonitor.h)
    cemu_use_precompiled_header(CemuNativeHost)
    target_include_directories(CemuNativeHost PRIVATE ${RAPIDJSON_INCLUDE_DIRS})
    target_link_libraries(CemuNativeHost PRIVATE LauncherStorage LauncherInstallation LauncherHistory CemuAudio CemuCafe CemuCommon CemuComponents CemuConfig CemuInput CemuUtil CemuGui CemuResource pugixml::pugixml libzip::zip OpenGL::GL Dbghelp)
    set_target_properties(LauncherShell PROPERTIES CEMU_NATIVE_HOST TRUE)
    target_compile_definitions(LauncherShell PRIVATE CEMU_NATIVE_HOST)
    target_link_libraries(LauncherShell PRIVATE CemuNativeHost windowscodecs ZLIB::ZLIB)
    target_link_options(LauncherShell PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/LauncherLicenses.cmake")
