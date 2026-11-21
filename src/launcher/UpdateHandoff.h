#pragma once
#include "UpdateSecurity.h"

namespace LauncherUpdate
{
struct Handoff
{
	std::string manifest, signature, release, targetId;
	unsigned protocol_version = 2;
	std::string nonce;
};
// 引数は製品から決定した Cache 内の固定ファイル名のみ。任意の実行パスを IPC に含めない。
File StartUpdater(const fs::path& transaction, const Handoff& handoff);
bool RunUpdater(HANDLE input, HANDLE parent, HANDLE acknowledgement,
	const std::function<void(State)>& status = {}, std::atomic_uint* applyGate = nullptr);
bool NotifyUpdateReady(std::wstring_view arguments, std::string_view version = {});
bool RunBootstrapper(std::wstring_view arguments = {});
} // namespace LauncherUpdate
