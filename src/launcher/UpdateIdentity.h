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
// 本番公開鍵は未設定。環境変数・設定・コマンドラインによる差替えを許可しない。
inline constexpr std::string_view TrustedPublicKey = "";
} // namespace LauncherUpdate
