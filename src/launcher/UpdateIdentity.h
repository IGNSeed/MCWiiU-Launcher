#pragma once

#include <string_view>

namespace LauncherUpdate
{
inline constexpr std::string_view ProductId = "MCWiiU-Launcher";
inline constexpr std::string_view ProductName = "MCWiiU Launcher";
inline constexpr std::string_view AppId = "{362BA210-0BBE-45ED-A0F5-6000DB57594B}";
#ifndef MCWIIU_FOUNDATION_VERSION
#define MCWIIU_FOUNDATION_VERSION "1.0.0"
#endif
inline constexpr std::string_view BootstrapperVersion = MCWIIU_FOUNDATION_VERSION;
inline constexpr unsigned ProtocolVersion = 2;
inline constexpr std::string_view TrustedSetupPublisher = "";
// 本番公開鍵。環境変数・設定・コマンドラインによる差替えを許可しない。
inline constexpr std::string_view TrustedPublicKey = "e4aa93d6ac191b3fc0f7c8cd721e6a7f3202ba64c7c9f91731464f20da3015b7";
} // namespace LauncherUpdate
