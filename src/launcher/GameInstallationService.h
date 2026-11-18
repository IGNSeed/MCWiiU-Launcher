#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class LauncherPaths;

// Core と GUI に依存しないタイトル管理。パスは native の選択結果だけを渡す。
class GameInstallationService final
{
public:
	using Path = std::filesystem::path;
	enum class Region { None, JPN, USA, EUR };
	enum class TitleType { Base, Update, DLC };
	enum class Operation { None, Install, ReinstallBase, ReinstallUpdate, ChangeRegion, Uninstall, RemoveDLC, Recover };
	enum class State { NotInstalled, BaseOnly, UpdateMissing, Ready, Installing, Reinstalling, ChangingRegion, Uninstalling, RecoveryRequired, Error };
	enum class Phase { Idle, Analyzing, Planning, Copying, Placing, RollingBack, Cleaning, Completed };
	enum class Error { None, Busy, GameActive, InvalidPath, UnsafePath, InvalidMetadata, UnsupportedTitle, UnsupportedVersion, RegionMismatch, AlreadyInstalled, NotInstalled, UnmanagedTitle, NoSpace, AccessDenied, CopyFailed, Cancelled, InvalidJournal, RecoveryRequired, IO };
	enum class Completion { None, Running, Succeeded, Cancelled, Failed };
	enum class Relation { NewTitle, Installed, OtherRegion };
	struct Result
	{
		Error error = Error::None;
		std::string message;
		explicit operator bool() const { return error == Error::None; }
	};
	struct Title
	{
		uint64_t id = 0;
		TitleType type = TitleType::Base;
		Region region = Region::None;
		uint16_t version = 0;
		Path source, destination;
		uint64_t bytes = 0, files = 0;
		Relation relation = Relation::NewTitle;
	};
	struct Analysis
	{
		Result result;
		Title title;
	};
	struct Request
	{
		Operation operation = Operation::Install;
		std::vector<Path> sources;
		// RemoveDLC はこの ID だけを操作する。Uninstall は全管理タイトルを対象にする。
		uint64_t dlcTitleId = 0;
	};
	struct Progress
	{
		Operation operation = Operation::None;
		Phase phase = Phase::Idle;
		uint64_t titleId = 0, totalBytes = 0, processedBytes = 0;
		double percent = 0;
		bool cancellable = false;
		Completion completion = Completion::None;
		Result result;
	};
	struct Snapshot
	{
		State state = State::NotInstalled;
		Region region = Region::None;
		std::vector<Title> titles;
		Path basePath;
		bool ready = false, launchSupported = false, legacyDataPresent = false;
		Result result;
		Progress progress;
	};

	explicit GameInstallationService(const LauncherPaths& paths);
	~GameInstallationService();
	GameInstallationService(const GameInstallationService&) = delete;
	GameInstallationService& operator=(const GameInstallationService&) = delete;

	// Core のタイトル一覧初期化前に呼ぶ。未完了取引はここで復旧する。
	Result Initialize();
	// ファイル走査を伴うため、UI では worker から呼ぶ。元データは変更しない。
	Analysis AnalyzeSource(const Path& selected) const;
	// 受付と PLAY 予約は同じ排他で行う。重い処理は所有 worker で実行する。
	Result Start(Request request);
	void Cancel();
	Snapshot GetSnapshot() const;
	bool TryBeginPlay();
	void EndPlay();
	void Wait();
	static Path TitlePath(const Path& data, uint64_t titleId);
	static const char* StateName(State state);

#ifdef GAME_INSTALLATION_TESTING
	// 専用 harness のみにリンクする。製品 EXE には注入経路を含めない。
	static void SetTestCheckpoint(std::function<void(std::string_view, uint64_t)> checkpoint);
#endif

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
