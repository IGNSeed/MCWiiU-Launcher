#pragma once

#include <array>
#include <cstdint>
#include <filesystem>

namespace MiiDatabase
{

enum class State { Missing, Valid, Invalid, ReadError, AccessDenied };
enum class InvalidReason { None, NotRegular, Size, Identifier, Crc };

struct File
{
	std::filesystem::path path;
	State state = State::ReadError;
	InvalidReason reason = InvalidReason::None;
	uint32_t nativeError = 0;
};

struct Inspection
{
	// HDB、ODB、ODB_OLD の順。ひとつが不足していても全ファイルを検査する。
	std::array<File, 3> files;
	bool HasBlockingError() const;
	bool HasRequiredDatabases() const;
	bool NeedsInitialization() const;
};

// miiTitleId は準備済み title の _SYSGetSystemApplicationTitleId(4) から取得する。
// 読み取り専用の検査。初期化、修復、ディレクトリ作成は行わない。
Inspection Inspect(const std::filesystem::path& mlc, uint64_t miiTitleId);

} // namespace MiiDatabase
