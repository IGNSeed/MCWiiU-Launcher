#pragma once

#include <zip.h>

// more secure and robust wrapper over libzip API. Also follows our utf8 conventions for std::string(_view). Read-only access for now
class ZipArchive
{
public:
	struct ZipFileEntry
	{
		std::string_view fullPath;
		std::string_view filename;
	};

	ZipArchive(const fs::path& path, bool strict = false)
	{
		m_zipPath = _pathToUtf8(path);
		int zeError{0};
		m_zip = zip_open(m_zipPath.c_str(), ZIP_RDONLY | (strict ? ZIP_CHECKCONS : 0), &zeError);
		if (!m_zip && !strict)
			cemuLog_log(LogType::Force, "Opening zip \"{}\" failed with error code {}", m_zipPath, zeError);
	}

	ZipArchive(const ZipArchive&) = delete;
	ZipArchive& operator=(const ZipArchive&) = delete;

	struct EntryInfo
	{
		std::string_view path;
		uint64 size = 0, compressedSize = 0;
		uint32 attributes = 0;
		uint8 operatingSystem = 0;
		uint16 encryption = 0;
	};

	bool GetEntryInfo(size_t index, EntryInfo& info) const
	{
		zip_stat_t stat{};
		uint8 os = 0;
		uint32 attributes = 0;
		constexpr auto required = ZIP_STAT_NAME | ZIP_STAT_SIZE | ZIP_STAT_COMP_SIZE | ZIP_STAT_ENCRYPTION_METHOD;
		if (!m_zip || zip_stat_index(m_zip, index, 0, &stat) != 0 || (stat.valid & required) != required ||
			zip_file_get_external_attributes(m_zip, index, 0, &os, &attributes) != 0) return false;
		info = {stat.name, stat.size, stat.comp_size, attributes, os, stat.encryption_method};
		return true;
	}

	// 非信頼 ZIP 用。宣言値と実読取量の両方を制限し、CRC エラーも失敗にする。
	bool ExtractIntoMemoryBounded(size_t index, std::vector<uint8>& data, size_t limit) const
	{
		data.clear();
		EntryInfo info;
		if (!GetEntryInfo(index, info) || info.size > limit) return false;
		auto* file = zip_fopen_index(m_zip, index, 0);
		if (!file) return false;
		bool ok = true;
		uint8 buffer[4096];
		for (;;)
		{
			const auto count = zip_fread(file, buffer, sizeof(buffer));
			if (count == 0) break;
			if (count < 0 || static_cast<uint64>(count) > limit - data.size()) { ok = false; break; }
			data.insert(data.end(), buffer, buffer + count);
		}
		ok = zip_fclose(file) == 0 && ok && data.size() == info.size;
		if (!ok) data.clear();
		return ok;
	}

	~ZipArchive()
	{
		if (m_zip)
			zip_close(m_zip);
	}

	bool IsValid()
	{
		return m_zip;
	}

	size_t GetNumEntries()
	{
		cemu_assert(m_zip); // zip must be valid to call this
		zip_int64_t num = zip_get_num_entries(m_zip, 0);
		if (num < 0)
			return 0;
		return static_cast<size_t>(num);
	}

	bool GetFileEntryByIndex(size_t index, ZipFileEntry& entry)
	{
		zip_stat_t st{};
		if (zip_stat_index(m_zip, (zip_uint64_t)index, 0, &st) != 0)
		{
			cemuLog_log(LogType::Force, "Zip has corrupted entry at index {} in zip {}", index, m_zipPath);
			return false;
		}
		// we only return entries with a valid name
		if (!(st.valid & ZIP_STAT_NAME))
			return false;
		size_t nameLen = strlen(st.name);
		if (nameLen == 0)
			return false;
		// reject "." and ".." path components
		{
			std::string_view path = st.name;
			size_t start = 0;
			while (start <= path.size())
			{
				size_t end = path.find('/', start);
				if (end == std::string_view::npos)
					end = path.size();
				const auto component = path.substr(start, end - start);
				if (component == "." || component == "..")
					return false;
				if (end == path.size())
					break;
				start = end + 1;
			}
		}
		// reject backward slash in path, its technically allowed on Linux and macOS, but for Windows this is a path delimiter
		// this also rejects UNC paths like \\server\share\file
		if (strchr(st.name, '\\'))
			return false;
		// reject absolute Unix paths
		if (st.name[0] == '/')
			return false;
		// reject Windows drive paths, e.g. C:/foo
		if (nameLen >= 2 &&
			((st.name[0] >= 'A' && st.name[0] <= 'Z') || (st.name[0] >= 'a' && st.name[0] <= 'z')) &&
			st.name[1] == ':')
		{
			return false;
		}
		// an entry may also be an explicit directory, indicated by a slash at the end
		// we do not return those as files either
		bool isDir = st.name[nameLen-1] == '/';
		if (isDir)
			return false;
		entry = {};
		entry.fullPath = st.name;
		// extract filename from full path
		auto pos = entry.fullPath.find_last_of('/');
		if (pos != std::string_view::npos)
			entry.filename = entry.fullPath.substr(pos+1);
		else
			entry.filename = entry.fullPath;
		return true;
	}

	bool ExtractIntoMemoryByIndex(size_t index, std::vector<uint8>& data)
	{
		zip_file_t* zf = zip_fopen_index(m_zip, index, 0);
		if (!zf)
		{
			cemuLog_log(LogType::Force, "Failed to open file {} in zip {}", index, m_zipPath);
			cemu_assert_suspicious();
			return false;
		}
		data.clear();
		uint8 tmpBuffer[1024*4];
		while (true)
		{
			// keep calling zip_fread until end of file is reached
			zip_int64_t r = zip_fread(zf, tmpBuffer, 1024*4);
			if (r == 0)
				break;
			if (r < 0)
			{
				cemuLog_log(LogType::Force, "Read error while reading file at index {} in zip {}", index, m_zipPath);
				cemu_assert_suspicious();
				zip_fclose(zf);
				return false;
			}
			data.insert(data.end(), tmpBuffer, tmpBuffer + r);
		}
		zip_fclose(zf);
		return true;
	}

private:
	zip_t* m_zip{};
	std::string m_zipPath;
};
