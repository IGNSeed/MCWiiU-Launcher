#pragma once

#include <filesystem>
#include <memory>
#include "Cafe/TitleList/TitleInfo.h"

namespace GameLaunch
{
	enum class Error
	{
		None,
		MissingBaseTitle,
		InvalidExecutable,
		UnableToMount,
		PrepareFailed,
		StandalonePrepareFailed,
		InvalidPath,
	};

	struct Preparation
	{
		Error error = Error::None;
		std::filesystem::path recentPath;
		bool missingDiscKey = false;
		bool missingTitleTicket = false;

	  private:
		// 従来どおり、UI の起動処理が戻るまで解析済み情報を保持する。
		std::unique_ptr<TitleInfo> m_titleInfo;
		friend Preparation Prepare(const std::filesystem::path& path);
	};

	// タイトルを準備するだけで、Canvas 生成や LaunchForegroundTitle は行わない。
	Preparation Prepare(const std::filesystem::path& path);
} // namespace GameLaunch
