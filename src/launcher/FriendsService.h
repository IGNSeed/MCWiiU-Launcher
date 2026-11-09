#pragma once
#include <string>
#include <string_view>
#include <cstdint>

// UI へ渡す Friends 情報を公開プロフィールだけに限定する。
std::string FriendsSnapshotJson();
std::string FriendsManagementCommand(uint64_t requestId, std::string_view payload);
