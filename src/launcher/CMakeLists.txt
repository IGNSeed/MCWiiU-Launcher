cmake_minimum_required(VERSION 3.25)
project(MCWiiULauncher VERSION 0.3.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "LauncherShell requires Windows x64 with MSVC")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(TARGET CemuCafe)
    # 既存 Core と vcpkg static の CRT を変えず、Launcher 側を合わせる。
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set(LAUNCHER_DEPENDENCY_CACHE "${CMAKE_BINARY_DIR}/_dependencies" CACHE PATH "Launcher dependency cache")

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

add_executable(LauncherShell WIN32 main.cpp NativeGameHost.cpp NativeGameHost.h LauncherShell.manifest)
target_compile_features(LauncherShell PRIVATE cxx_std_23)
target_compile_options(LauncherShell PRIVATE /utf-8 /W4)
target_compile_definitions(LauncherShell PRIVATE NOMINMAX)
# stable native API が参照する既存 WebView2 SDK も引き継ぐ。
saucer_add_module(LauncherShell)
target_link_libraries(LauncherShell PRIVATE Comctl32)
set_target_properties(LauncherShell PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
saucer_embed("frontend" TARGET LauncherShell DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/embedded")
target_link_libraries(LauncherShell PRIVATE saucer::embedded)

if(TARGET CemuCafe)
    target_compile_options(LauncherShell PRIVATE /UWIN32_LEAN_AND_MEAN)
    add_library(CemuNativeHost STATIC CemuHost.cpp CemuHost.h ../Cemu/Startup.cpp ../Cemu/GameLaunch.cpp)
    set_target_properties(CemuNativeHost PROPERTIES CXX_STANDARD 20 CEMU_NATIVE_HOST TRUE)
    cemu_use_precompiled_header(CemuNativeHost)
    target_include_directories(CemuNativeHost PRIVATE ${RAPIDJSON_INCLUDE_DIRS})
    target_link_libraries(CemuNativeHost PRIVATE CemuAudio CemuCafe CemuCommon CemuComponents CemuConfig CemuInput CemuUtil CemuGui CemuResource pugixml::pugixml OpenGL::GL Dbghelp)
    set_target_properties(LauncherShell PROPERTIES CEMU_NATIVE_HOST TRUE)
    target_compile_definitions(LauncherShell PRIVATE CEMU_NATIVE_HOST)
    target_link_libraries(LauncherShell PRIVATE CemuNativeHost)
    target_link_options(LauncherShell PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO)
endif()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bin/licenses")
configure_file("${saucer_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/bin/licenses/saucer-LICENSE" COPYONLY)
