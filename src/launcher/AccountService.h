#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

class AccountService final
{
public:
	AccountService();
	~AccountService();
	AccountService(const AccountService&) = delete;
	AccountService& operator=(const AccountService&) = delete;
	std::string Command(std::string_view payload, bool canEdit);
	std::string PreviewZip(const std::filesystem::path& selectedFile, bool canEdit);
	bool IsHealthy() const;
	bool IsTesting() const;
	void PrepareLaunch();
	void RunConnectionTest();
	std::string ImportCustom(const std::filesystem::path& selectedFile, bool canEdit, bool replaceExisting);
	static void EnforceOffline();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
