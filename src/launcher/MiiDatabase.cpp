#include "MiiDatabase.h"

#include <Windows.h>
#include <algorithm>
#include <format>
#include <string_view>

namespace
{

	struct FileHandle
	{
		HANDLE value;
		~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
	};

	void SetReadError(MiiDatabase::File& file, DWORD error)
	{
		file.nativeError = error;
		if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
			file.state = MiiDatabase::State::Missing;
		else if (error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD)
			file.state = MiiDatabase::State::AccessDenied;
		else
			file.state = MiiDatabase::State::ReadError;
	}

	MiiDatabase::File InspectFile(const std::filesystem::path& path, std::string_view identifier, DWORD expectedSize)
	{
		MiiDatabase::File result{path};
		// 検査中の書込み・置換を共有しない。既存 writer との競合も成功扱いしない。
		FileHandle handle{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
			FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
		if (handle.value == INVALID_HANDLE_VALUE)
		{
			SetReadError(result, GetLastError());
			return result;
		}
		BY_HANDLE_FILE_INFORMATION information{};
		if (!GetFileInformationByHandle(handle.value, &information))
		{
			SetReadError(result, GetLastError());
			return result;
		}
		result.state = MiiDatabase::State::Invalid;
		if ((information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || information.nNumberOfLinks != 1)
		{
			result.reason = MiiDatabase::InvalidReason::NotRegular;
			return result;
		}
		// 対象形式は v560 FFL の固定長 DB。未知の形式を初期化可能と判定しない。
		if (information.nFileSizeHigh || information.nFileSizeLow != expectedSize)
		{
			result.reason = MiiDatabase::InvalidReason::Size;
			return result;
		}
		std::array<unsigned char, 32768> buffer{};
		uint16_t crc = 0;
		DWORD remaining = expectedSize;
		bool first = true;
		while (remaining)
		{
			DWORD read = 0;
			const auto count = std::min<DWORD>(remaining, static_cast<DWORD>(buffer.size()));
			if (!ReadFile(handle.value, buffer.data(), count, &read, nullptr))
			{
				SetReadError(result, GetLastError());
				return result;
			}
			if (read != count)
			{
				SetReadError(result, ERROR_HANDLE_EOF);
				return result;
			}
			if (first && std::string_view(reinterpret_cast<const char*>(buffer.data()), identifier.size()) != identifier)
			{
				result.reason = MiiDatabase::InvalidReason::Identifier;
				return result;
			}
			first = false;
			// FFLiGetCRC16: 初期値0、各byteの前に8回shiftしてからbyteをxorする。
			// FFLiIsValidCRC16 は末尾CRCを含むファイル全体の剰余0を検査する。
			for (DWORD index = 0; index < read; ++index)
			{
				for (unsigned bit = 0; bit < 8; ++bit)
					crc = static_cast<uint16_t>((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
				crc ^= buffer[index];
			}
			remaining -= read;
		}
		if (crc)
			result.reason = MiiDatabase::InvalidReason::Crc;
		else
			result.state = MiiDatabase::State::Valid;
		return result;
	}

} // namespace

namespace MiiDatabase
{

bool Inspection::HasBlockingError() const
{
	return std::any_of(files.begin(), files.end(), [](const auto& file) { return file.state != State::Missing && file.state != State::Valid; });
}

bool Inspection::HasRequiredDatabases() const
{
	return !HasBlockingError() && files[0].state == State::Valid && files[1].state == State::Valid;
}

bool Inspection::NeedsInitialization() const
{
	return !HasBlockingError() && (files[0].state == State::Missing || files[1].state == State::Missing);
}

Inspection Inspect(const std::filesystem::path& mlc, uint64_t miiTitleId)
{
	const auto directory = mlc / std::format(L"usr/save/{:08x}/{:08x}/user/common/db", static_cast<uint32_t>(miiTitleId >> 32), static_cast<uint32_t>(miiTitleId));
	return {{{InspectFile(directory / L"FFL_HDB.dat", "FFHA", 44032),
		InspectFile(directory / L"FFL_ODB.dat", "FFOC", 276544),
		InspectFile(directory / L"FFL_ODB_OLD.dat", "FFOC", 276544)}}};
}

} // namespace MiiDatabase
