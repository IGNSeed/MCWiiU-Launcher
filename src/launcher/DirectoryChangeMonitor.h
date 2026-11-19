#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <vector>

// 呼出側の共通同期周期で通知を回収する。監視ごとのスレッドは作らない。
class DirectoryChangeMonitor final
{
public:
	DirectoryChangeMonitor();
	~DirectoryChangeMonitor();
	void SetPaths(std::vector<std::filesystem::path> paths);
	bool Poll(bool canInspect);
	bool IsAvailable() const;
	bool HasPending() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
