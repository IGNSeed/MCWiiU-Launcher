#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

// Coreから独立したworking MLCと不足DBの追加transaction。既存DBは削除しない。
class MiiInitializationStorage
{
public:
	using Cancel = std::function<bool()>;
	static void Recover(const std::filesystem::path& data);
	MiiInitializationStorage(const std::filesystem::path& data, const std::vector<std::filesystem::path>& titles, Cancel cancel);
	~MiiInitializationStorage();
	MiiInitializationStorage(const MiiInitializationStorage&) = delete;
	MiiInitializationStorage& operator=(const MiiInitializationStorage&) = delete;
	const std::filesystem::path& WorkingMlc() const;
	void Commit(Cancel cancel);
private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
