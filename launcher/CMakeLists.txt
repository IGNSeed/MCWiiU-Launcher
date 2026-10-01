if (CMAKE_VERSION VERSION_LESS 3.31)
    message(FATAL_ERROR "MCWiiU Launcher requires CMake 3.31 or newer for Saucer's embedding helper")
endif()
if (NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
    CMAKE_GENERATOR_PLATFORM MATCHES "^[Aa][Rr][Mm]" OR
    CMAKE_SYSTEM_PROCESSOR MATCHES "[Aa][Rr][Mm]|[Aa][Aa][Rr][Cc][Hh]")
    message(FATAL_ERROR "MCWiiU Launcher supports Windows x64 only")
endif()

include(FetchContent)
mcwiiu_require_external_path("${FETCHCONTENT_BASE_DIR}")
if (CPM_SOURCE_CACHE)
    mcwiiu_require_external_path("${CPM_SOURCE_CACHE}")
elseif (DEFINED ENV{CPM_SOURCE_CACHE} AND NOT "$ENV{CPM_SOURCE_CACHE}" STREQUAL "")
    mcwiiu_require_external_path("$ENV{CPM_SOURCE_CACHE}")
endif()
set(saucer_backend "WebView2" CACHE STRING "" FORCE)
set(saucer_webview2_arch "x64" CACHE STRING "" FORCE)
set(saucer_examples OFF CACHE BOOL "" FORCE)
set(saucer_tests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(PackageProject
    GIT_REPOSITORY https://github.com/TheLartians/PackageProject.cmake.git
    GIT_TAG 738d2ed9dba67e01ac2ea2a6d0f8d35c198fed32 # v1.13.0
    PATCH_COMMAND ${CMAKE_COMMAND}
        "-DPROJECT_ROOT=${CMAKE_SOURCE_DIR}" "-DSOURCE_DIR=<SOURCE_DIR>"
        -P "${CMAKE_SOURCE_DIR}/cmake/PatchPackageProject.cmake"
)
FetchContent_Declare(saucer
    GIT_REPOSITORY https://github.com/saucer/saucer.git
    GIT_TAG 68345fa45a7465fe5a62b699b51847b916c8c16a # v8.0.5
)
FetchContent_MakeAvailable(saucer)

add_library(MCWiiUCemuBackend STATIC
    native/runtime/cemu/CemuRuntimeBackend.cpp
    native/runtime/cemu/EmbeddedWindowSystem.cpp
)
target_include_directories(MCWiiUCemuBackend PUBLIC native)
target_compile_features(MCWiiUCemuBackend PUBLIC cxx_std_20)
target_precompile_headers(MCWiiUCemuBackend PRIVATE "${CMAKE_SOURCE_DIR}/src/Common/precompiled.h")
target_link_libraries(MCWiiUCemuBackend PRIVATE
    CemuCafe CemuCommon CemuComponents CemuConfig CemuAudio CemuInput CemuUtil glm::glm dbghelp
)
cemu_set_msvc_runtime(MCWiiUCemuBackend)

add_library(MCWiiUFoundation STATIC
    native/runtime/MinecraftRuntime.cpp
    native/bridge/LauncherBridge.cpp
    native/platform/windows/UserDataPath.cpp
)
target_include_directories(MCWiiUFoundation PUBLIC native)
target_compile_features(MCWiiUFoundation PUBLIC cxx_std_20)
target_compile_definitions(MCWiiUFoundation PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(MCWiiUFoundation PRIVATE MCWiiUCemuBackend shell32 ole32)
cemu_set_msvc_runtime(MCWiiUFoundation)
target_compile_definitions(MCWiiUFoundation PUBLIC
    MCWIIU_DEVELOPMENT=$<IF:$<CONFIG:Debug,RelWithDebInfo>,1,0>
    MCWIIU_BUILD_CONFIGURATION="$<CONFIG>"
)

add_executable(MCWiiULauncher WIN32
    native/app/Main.cpp
)
cemu_set_msvc_runtime(MCWiiULauncher)
target_compile_features(MCWiiULauncher PRIVATE cxx_std_23)
set_target_properties(MCWiiULauncher PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
    OUTPUT_NAME "MCWiiU Launcher"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)
target_compile_definitions(MCWiiULauncher PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
if (MSVC)
    target_compile_options(MCWiiULauncher PRIVATE /W4 /utf-8 /EHsc)
    target_compile_options(MCWiiUFoundation PRIVATE /W4 /utf-8 /EHsc)
endif()

include("${CMAKE_SOURCE_DIR}/cmake/LauncherFrontend.cmake")
mcwiiu_build_frontend("${CMAKE_CURRENT_SOURCE_DIR}/frontend" "${CMAKE_CURRENT_BINARY_DIR}/frontend")
saucer_embed("${CMAKE_CURRENT_BINARY_DIR}/frontend/dist"
    DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/embedded" TARGET MCWiiULauncher
)
target_link_libraries(MCWiiULauncher PRIVATE
    MCWiiUFoundation saucer::saucer saucer::embedded shell32 ole32
)
install(TARGETS MCWiiULauncher RUNTIME DESTINATION . COMPONENT Launcher)
