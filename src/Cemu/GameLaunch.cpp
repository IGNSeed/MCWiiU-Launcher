#include "Cemu/GameLaunch.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/TitleList/TitleList.h"

namespace GameLaunch
{
	const char* ErrorName(Error error)
	{
		switch (error)
		{
		case Error::None: return "None";
		case Error::MissingBaseTitle: return "MissingBaseTitle";
		case Error::InvalidExecutable: return "InvalidExecutable";
		case Error::UnableToMount: return "UnableToMount";
		case Error::PrepareFailed: return "PrepareFailed";
		case Error::StandalonePrepareFailed: return "StandalonePrepareFailed";
		case Error::InvalidPath: return "InvalidPath";
		}
		return "Unknown";
	}

	Preparation Prepare(const std::filesystem::path& path)
	{
		Preparation result;
		result.m_titleInfo = std::make_unique<TitleInfo>(path);
		auto& title = *result.m_titleInfo;
		if (title.IsValid())
		{
			CafeTitleList::AddTitleFromPath(path);
			TitleId baseTitleId;
			if (!CafeTitleList::FindBaseTitleId(title.GetAppTitleId(), baseTitleId))
			{
				result.error = Error::MissingBaseTitle;
				return result;
			}
			const auto status = CafeSystem::PrepareForegroundTitle(baseTitleId);
			if (status == CafeSystem::PREPARE_STATUS_CODE::INVALID_RPX)
				result.error = Error::InvalidExecutable;
			else if (status == CafeSystem::PREPARE_STATUS_CODE::UNABLE_TO_MOUNT)
				result.error = Error::UnableToMount;
			else if (status != CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
				result.error = Error::PrepareFailed;
			if (result.error == Error::None)
				result.recentPath = title.GetPath();
			return result;
		}

		const auto fileType = DetermineCafeSystemFileType(path);
		if (fileType == CafeTitleFileType::RPX || fileType == CafeTitleFileType::ELF)
		{
			if (CafeSystem::PrepareForegroundTitleFromStandaloneRPX(path) != CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
				result.error = Error::StandalonePrepareFailed;
			else
				result.recentPath = path;
			return result;
		}

		result.error = Error::InvalidPath;
		result.missingDiscKey = title.GetInvalidReason() == TitleInfo::InvalidReason::NO_DISC_KEY;
		result.missingTitleTicket = title.GetInvalidReason() == TitleInfo::InvalidReason::NO_TITLE_TIK;
		return result;
	}
} // namespace GameLaunch
