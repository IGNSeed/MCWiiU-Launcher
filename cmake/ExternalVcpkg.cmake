find_package(Git REQUIRED)
set(MCWIIU_VCPKG_ROOT "${CMAKE_BINARY_DIR}/vcpkg-source")
mcwiiu_require_external_path("${MCWIIU_VCPKG_ROOT}")
foreach(cache_name VCPKG_DOWNLOADS VCPKG_DEFAULT_BINARY_CACHE)
    if (DEFINED ENV{${cache_name}})
        mcwiiu_require_external_path("$ENV{${cache_name}}")
        file(MAKE_DIRECTORY "$ENV{${cache_name}}")
    endif()
endforeach()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}/dependencies/vcpkg" rev-parse HEAD
    OUTPUT_VARIABLE vcpkg_revision OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
if (NOT EXISTS "${MCWIIU_VCPKG_ROOT}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" clone --local --no-checkout
            "${CMAKE_SOURCE_DIR}/dependencies/vcpkg" "${MCWIIU_VCPKG_ROOT}"
        COMMAND_ERROR_IS_FATAL ANY
    )
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${MCWIIU_VCPKG_ROOT}" checkout --detach "${vcpkg_revision}"
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${MCWIIU_VCPKG_ROOT}" rev-parse HEAD
    OUTPUT_VARIABLE staged_revision OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
if (NOT staged_revision STREQUAL vcpkg_revision)
    message(FATAL_ERROR "External vcpkg checkout differs from the pinned submodule. Use a fresh external build tree.")
endif()
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${MCWIIU_VCPKG_ROOT}" rev-parse --is-shallow-repository
    OUTPUT_VARIABLE vcpkg_shallow OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
if (vcpkg_shallow STREQUAL "true")
    # CI may supply shallow submodules. Versioned ports need baseline history;
    # fetch it only in the disposable checkout, preserving the source and pin.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}/dependencies/vcpkg" remote get-url origin
        OUTPUT_VARIABLE vcpkg_origin OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )
    message(STATUS "Fetching pinned vcpkg history into the external checkout")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${MCWIIU_VCPKG_ROOT}" fetch --unshallow "${vcpkg_origin}" "${vcpkg_revision}"
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()
if (DEFINED CMAKE_TOOLCHAIN_FILE)
    mcwiiu_require_external_path("${CMAKE_TOOLCHAIN_FILE}")
endif()
