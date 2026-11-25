#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "WorldImport.h"

// Core の状態と Account の検証は呼出側の同じロック内で行う。
class WorldService final
{
public:
	struct Account
	{
		uint32_t id;
		std::string name;
	};
	struct Context
	{
		uint32_t activeId;
		uint64_t accountRevision;
		bool canEdit;
		std::vector<Account> accounts;
		bool canManageAll = false; // 移動・既存アイコン変更・復旧・Trash 編集は Ready 時だけ許可する。
	};
	WorldService(std::filesystem::path configRoot, std::filesystem::path saveRoot, uint64_t titleId = 0x00050000101dbe00ULL, uint64_t initialRevision = 1);
	~WorldService();
	bool IsHealthy() const;
	std::vector<std::filesystem::path> WatchPaths(uint32_t activeId) const;
	std::string OpenFolder(std::string_view payload, const Context& context, const std::function<bool(const std::filesystem::path&)>& open);
	std::string Command(std::string_view payload, const Context& context);
	std::string PreviewImage(std::string_view payload, const std::filesystem::path& selectedFile, const Context& context);
	std::string PreviewImport(std::string_view payload, const std::filesystem::path& selected, std::string_view part, const Context& context);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
