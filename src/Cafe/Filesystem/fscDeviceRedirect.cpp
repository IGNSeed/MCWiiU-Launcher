#include "util/helpers/helpers.h"
#include "Cafe/Filesystem/fscDeviceHostFS.h"
#include "FST/fstUtil.h"

struct RedirectEntry
{
	RedirectEntry(const fs::path& dstPath, sint32 priority) : dstPath(dstPath), priority(priority) {}
	fs::path dstPath;
	sint32 priority;
};

FSAFileTree<RedirectEntry> redirectTree;

namespace
{
	std::map<const void*, std::map<std::string, FSCOwnedRedirect>> s_ownedRedirects;
	std::set<std::string> s_legacyPaths;
	std::string RedirectKey(std::string_view path)
	{
		std::string normalized(path);
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		FSCPath parsed(normalized);
		std::string result;
		for (size_t i = 0; i < parsed.GetNodeCount(); ++i)
		{
			result += '/';
			for (char c : parsed.GetNodeName(i)) result += c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
		}
		return result;
	}
}

extern std::recursive_mutex s_fscMutex;

void fscDeviceRedirect_add(std::string_view virtualSourcePath, size_t fileSize, const fs::path& targetFilePath, sint32 priority)
{
	std::lock_guard lock(s_fscMutex);
	const auto key = RedirectKey(virtualSourcePath);
	for (const auto& [owner, files] : s_ownedRedirects)
		if (files.contains(key)) throw std::runtime_error("File replacement conflict at " + key);
	s_legacyPaths.insert(key);
	// check if source already has a redirection
	RedirectEntry* existingEntry;
	if (redirectTree.getFile(virtualSourcePath, existingEntry))
	{
		if (existingEntry->priority >= priority)
			return; // dont replace entries with equal or higher priority
		// unregister existing entry
		redirectTree.removeFile(virtualSourcePath);
		delete existingEntry;
	}
	RedirectEntry* entry = new RedirectEntry(targetFilePath, priority);
	redirectTree.addFile(virtualSourcePath, fileSize, entry);
}

class fscDeviceTypeRedirect : public fscDeviceC
{
	FSCVirtualFile* fscDeviceOpenByPath(std::string_view path, FSC_ACCESS_FLAG accessFlags, void* ctx, sint32* fscStatus) override
	{
		RedirectEntry* redirectionEntry;
		// fsc_open が FSC mutex を保持するため、解除とファイルを開く操作は競合しない。
		if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE))
		{
			const auto key = RedirectKey(path);
			for (const auto& [owner, files] : s_ownedRedirects)
			{
				const auto it = files.find(key);
				if (it != files.end())
				{
					// 書込み要求を下位の BASE/UPD に流さず、置換側も読み取り専用で開く。
					return FSCVirtualFile_Host::OpenFile(it->second.targetPath, FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, *fscStatus);
				}
			}
		}

		if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE) && redirectTree.getFile(path, redirectionEntry))
			return FSCVirtualFile_Host::OpenFile(redirectionEntry->dstPath, accessFlags, *fscStatus);

		FSCVirtualFile* dirIterator;

		if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_DIR) && redirectTree.getDirectory(path, dirIterator))
			return dirIterator;

		return nullptr;
	}

public:
	static fscDeviceTypeRedirect& instance()
	{
		static fscDeviceTypeRedirect _instance;
		return _instance;
	}
};

bool _redirectMapped = false;

void fscDeviceRedirect_map()
{
	if (_redirectMapped)
		return;
	_redirectMapped = fsc_mount("/", "/", &fscDeviceTypeRedirect::instance(), nullptr, FSC_PRIORITY_REDIRECT) == FSC_STATUS_OK;
}

namespace
{
	bool RegisterOwned(const void* owner, const std::vector<FSCOwnedRedirect>& files, bool pack, std::string& conflict)
	{
		std::lock_guard lock(s_fscMutex);
		if (!owner || s_ownedRedirects.contains(owner)) return false;
		std::map<std::string, FSCOwnedRedirect> prepared;
		for (const auto& file : files)
		{
			const auto key = RedirectKey(file.virtualPath);
			conflict = key;
			const bool valid = pack ? key.starts_with("/vol/content/") || key.starts_with("/internal/") || key.starts_with("/vol/aoc0005000c") : key.starts_with("/vol/content/common/");
			if (!valid || key.find("/../") != std::string::npos || key.ends_with("/..") || !file.targetPath.is_absolute() || !prepared.emplace(key, file).second || s_legacyPaths.contains(key)) return false;
			for (const auto& [other, entries] : s_ownedRedirects) if (entries.contains(key)) return false;
		}
		if (prepared.empty()) return true;
		fscDeviceRedirect_map();
		if (!_redirectMapped) return false;
		s_ownedRedirects.emplace(owner, std::move(prepared));
		conflict.clear();
		return true;
	}
}

bool fscDeviceRedirect_setOwned(const void* owner, const std::vector<FSCOwnedRedirect>& files)
{
	std::string conflict;
	return RegisterOwned(owner, files, false, conflict);
}

bool fscDeviceRedirect_setOwnedPack(const void* owner, const std::vector<FSCOwnedRedirect>& files, std::string& conflict)
{
	return RegisterOwned(owner, files, true, conflict);
}

void fscDeviceRedirect_removeOwned(const void* owner)
{
	std::lock_guard lock(s_fscMutex);
	s_ownedRedirects.erase(owner);
	if (s_ownedRedirects.empty() && s_legacyPaths.empty() && _redirectMapped)
	{
		fsc_unmount("/", FSC_PRIORITY_REDIRECT);
		_redirectMapped = false;
	}
}

size_t fscDeviceRedirect_ownedCount(const void* owner)
{
	std::lock_guard lock(s_fscMutex);
	const auto it = s_ownedRedirects.find(owner);
	return it == s_ownedRedirects.end() ? 0 : it->second.size();
}
