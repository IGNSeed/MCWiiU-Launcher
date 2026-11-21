# SDK は公式 Developer Portal から取得した未改変の配布物をローカルに配置する。
set(MCWIIU_DISCORD_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.local/dependencies/discord-social-sdk" CACHE PATH "Official Discord Social SDK directory")
set(MCWIIU_DISCORD_SDK_NOTICES "${MCWIIU_DISCORD_SDK_ROOT}/License-Notices.txt" CACHE FILEPATH "Original notices supplied with the Discord Social SDK")
# SDK 未導入時の旧 default だけを、実配布物の名前に移行する。
if(MCWIIU_DISCORD_SDK_NOTICES STREQUAL "${MCWIIU_DISCORD_SDK_ROOT}/LICENSE" AND NOT EXISTS "${MCWIIU_DISCORD_SDK_NOTICES}")
    set(MCWIIU_DISCORD_SDK_NOTICES "${MCWIIU_DISCORD_SDK_ROOT}/License-Notices.txt" CACHE FILEPATH "Original notices supplied with the Discord Social SDK" FORCE)
endif()
set(discord_sdk_header "${MCWIIU_DISCORD_SDK_ROOT}/include/discordpp.h")
set(discord_sdk_import "${MCWIIU_DISCORD_SDK_ROOT}/lib/release/discord_partner_sdk.lib")
set(discord_sdk_runtime "${MCWIIU_DISCORD_SDK_ROOT}/bin/release/discord_partner_sdk.dll")
foreach(required IN ITEMS "${discord_sdk_header}" "${discord_sdk_import}" "${discord_sdk_runtime}")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Official Discord Social SDK is required: ${required}. Download C++ SDK from https://discord.com/developers/applications/1573624989045887027/social-sdk/downloads and set MCWIIU_DISCORD_SDK_ROOT. The SDK is not downloaded or substituted automatically.")
    endif()
endforeach()
if(NOT EXISTS "${MCWIIU_DISCORD_SDK_NOTICES}")
    message(FATAL_ERROR "Set MCWIIU_DISCORD_SDK_NOTICES to the original notices supplied with the official SDK. Review its redistribution terms before packaging.")
endif()

add_library(DiscordSocialSdk SHARED IMPORTED)
set_target_properties(DiscordSocialSdk PROPERTIES
    IMPORTED_IMPLIB "${discord_sdk_import}"
    IMPORTED_LOCATION "${discord_sdk_runtime}"
    INTERFACE_INCLUDE_DIRECTORIES "${MCWIIU_DISCORD_SDK_ROOT}/include")

function(mcwiiu_configure_discord_presence)
    target_sources(LauncherShell PRIVATE DiscordPresence.cpp DiscordPresence.h DiscordPresenceState.h)
    target_link_libraries(LauncherShell PRIVATE DiscordSocialSdk Delayimp)
    target_link_options(LauncherShell PRIVATE /DELAYLOAD:discord_partner_sdk.dll)
    set_property(TARGET LauncherShell APPEND PROPERTY LINK_DEPENDS "${discord_sdk_runtime}" "${MCWIIU_DISCORD_SDK_NOTICES}")
    add_custom_command(TARGET LauncherShell POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:LauncherShell>/licenses"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${discord_sdk_runtime}" "$<TARGET_FILE_DIR:LauncherShell>/discord_partner_sdk.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MCWIIU_DISCORD_SDK_NOTICES}" "$<TARGET_FILE_DIR:LauncherShell>/licenses/Discord-Social-SDK-NOTICES.txt"
        VERBATIM)
endfunction()
