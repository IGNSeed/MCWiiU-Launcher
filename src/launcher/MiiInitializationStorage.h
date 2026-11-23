#pragma once

#include <filesystem>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <string>

// Coreから独立したworking MLCと不足DBの追加transaction。既存DBは削除しない。
class MiiInitializationStorage
{
public:
	using Cancel = std::function<bool()>;
	struct CleanupResult
	{
		uint64_t bytes = 0;
		uint32_t pending = 0;
		std::string reason;
	};
	static CleanupResult Recover(const std::filesystem::path& data);
	MiiInitializationStorage(const std::filesystem::path& data, const std::vector<std::filesystem::path>& titles, Cancel cancel);
	~MiiInitializationStorage();
	MiiInitializationStorage(const MiiInitializationStorage&) = delete;
	MiiInitializationStorage& operator=(const MiiInitializationStorage&) = delete;
	const std::filesystem::path& WorkingMlc() const;
	void PrepareGuestDirectories(uint32_t persistentId);
	void ObserveCreated(const std::filesystem::path& path, void* file) noexcept;
	void Commit(Cancel cancel);
	CleanupResult Cleanup(Cancel cancel = {});
	CleanupResult Abandon();
private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
