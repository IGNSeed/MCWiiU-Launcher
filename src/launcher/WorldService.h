#pragma once

#include <cstdint>
#include <filesystem>
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
	};
	WorldService(std::filesystem::path configRoot, std::filesystem::path saveRoot);
	~WorldService();
	bool IsHealthy() const;
	std::string Command(std::string_view payload, const Context& context);
	std::string PreviewImage(std::string_view payload, const std::filesystem::path& selectedFile, const Context& context);
	std::string PreviewImport(std::string_view payload, const std::filesystem::path& selected, WorldImport::Kind kind, const Context& context);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
