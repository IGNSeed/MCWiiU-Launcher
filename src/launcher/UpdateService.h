#pragma once
#include "UpdateSecurity.h"
#include <mutex>
#include <thread>

class UpdateService final
{
public:
	explicit UpdateService(std::filesystem::path cache);
	~UpdateService();
	void Check();
	void Approve(std::string_view targetId);
	void Cancel();
	void OpenFoundationReleases();
	void BeginWhenSafe(bool safe);
	void Install(std::string_view targetId);
	bool BlocksPlay() const;
	bool WantsSafeStop() const;
	bool WantsClose() const;
	std::string Json() const;

private:
	void Move(LauncherUpdate::State state);
	void Start(std::function<void()> operation);
	void CheckWorker();
	void DownloadWorker();
	void ConfirmApproval();
	std::filesystem::path m_cache, m_transaction;
	mutable std::mutex m_mutex;
	std::thread m_worker;
	std::atomic_bool m_cancel{false}, m_working{false};
	LauncherUpdate::State m_state = LauncherUpdate::State::Idle;
	LauncherUpdate::Candidate m_candidate;
	std::string m_release, m_error;
	uint64_t m_generation = 0, m_downloaded = 0;
	bool m_close = false;
	LauncherUpdate::File m_updater;
	std::unique_ptr<LauncherUpdate::PathLocks> m_cacheLocks;
};
