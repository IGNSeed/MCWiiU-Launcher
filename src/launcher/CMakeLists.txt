cmake_minimum_required(VERSION 3.25)
project(MCWiiULauncher VERSION 0.3.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "LauncherShell requires Windows x64 with MSVC")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
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

add_executable(LauncherShell WIN32 main.cpp)
target_compile_features(LauncherShell PRIVATE cxx_std_23)
target_compile_options(LauncherShell PRIVATE /utf-8 /W4)
target_link_libraries(LauncherShell PRIVATE saucer::saucer)
set_target_properties(LauncherShell PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
saucer_embed("frontend" TARGET LauncherShell DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/embedded")
target_link_libraries(LauncherShell PRIVATE saucer::embedded)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bin/licenses")
configure_file("${saucer_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/bin/licenses/saucer-LICENSE" COPYONLY)
