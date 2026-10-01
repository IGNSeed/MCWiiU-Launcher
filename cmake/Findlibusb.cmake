# SPDX-FileCopyrightText: 2022 Andrea Pappacoda <andrea@pappacoda.it>
# SPDX-License-Identifier: ISC

if (WIN32 AND ENABLE_VCPKG)
    # Native discovery preserves spaces that Windows pkgconf splits in -I/-L output.
    set(libusb_prefix "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    find_path(libusb_INCLUDE_DIR NAMES libusb.h
            PATHS "${libusb_prefix}/include/libusb-1.0" NO_DEFAULT_PATH)
    find_library(libusb_LIBRARY_RELEASE NAMES libusb-1.0
            PATHS "${libusb_prefix}/lib" NO_DEFAULT_PATH)
    find_library(libusb_LIBRARY_DEBUG NAMES libusb-1.0
            PATHS "${libusb_prefix}/debug/lib" NO_DEFAULT_PATH)
    find_package_handle_standard_args(libusb REQUIRED_VARS
            libusb_INCLUDE_DIR libusb_LIBRARY_RELEASE libusb_LIBRARY_DEBUG)
    mark_as_advanced(libusb_INCLUDE_DIR libusb_LIBRARY_RELEASE libusb_LIBRARY_DEBUG)
    if (libusb_FOUND AND NOT TARGET libusb::libusb)
        add_library(libusb::libusb UNKNOWN IMPORTED)
        set_target_properties(libusb::libusb PROPERTIES
                IMPORTED_CONFIGURATIONS "Debug;Release"
                IMPORTED_LOCATION "${libusb_LIBRARY_RELEASE}"
                IMPORTED_LOCATION_DEBUG "${libusb_LIBRARY_DEBUG}"
                IMPORTED_LOCATION_RELEASE "${libusb_LIBRARY_RELEASE}"
                MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
                MAP_IMPORTED_CONFIG_MINSIZEREL Release
                INTERFACE_INCLUDE_DIRECTORIES "${libusb_INCLUDE_DIR}")
    endif ()
    return()
endif ()

find_package(libusb CONFIG)
if (NOT libusb_FOUND)
    find_package(PkgConfig)
    if (PKG_CONFIG_FOUND)
        pkg_search_module(libusb IMPORTED_TARGET GLOBAL libusb-1.0 libusb)
        if (libusb_FOUND)
            add_library(libusb::libusb ALIAS PkgConfig::libusb)
        endif ()
    endif ()
endif ()

find_package_handle_standard_args(libusb
        REQUIRED_VARS
        libusb_LINK_LIBRARIES
        libusb_FOUND
        VERSION_VAR libusb_VERSION
)
