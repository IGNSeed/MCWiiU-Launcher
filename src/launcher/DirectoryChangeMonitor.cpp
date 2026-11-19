#include "DirectoryChangeMonitor.h"

#include <Windows.h>
#include <array>
#include <optional>
#include <stdexcept>
#include <string>

namespace
{
	using Clock = std::chrono::steady_clock;
	constexpr auto s_quiet = std::chrono::milliseconds(250);
	constexpr auto s_retry = std::chrono::seconds(5);
	bool Related(const std::filesystem::path& left, const std::filesystem::path& right)
	{
		const auto a = left.lexically_normal().wstring(), b = right.lexically_normal().wstring();
		const auto prefix = [](const std::wstring& path, const std::wstring& root) {
			return path.size() >= root.size() && _wcsnicmp(path.c_str(), root.c_str(), root.size()) == 0 &&
				(path.size() == root.size() || path[root.size()] == L'\\');
		};
		return prefix(a, b) || prefix(b, a);
	}
	struct Watch
	{
		std::filesystem::path target, directory;
		bool parent = false;
		HANDLE file = INVALID_HANDLE_VALUE;
		OVERLAPPED operation{};
		alignas(DWORD) std::array<BYTE, 32768> buffer{};
		Clock::time_point retry{};
		~Watch() { Close(); }
		void Close()
		{
			if (file != INVALID_HANDLE_VALUE)
			{
				CancelIoEx(file, &operation);
				DWORD bytes = 0;
				GetOverlappedResult(file, &operation, &bytes, TRUE);
				CloseHandle(file); file = INVALID_HANDLE_VALUE;
			}
			if (operation.hEvent) CloseHandle(operation.hEvent);
			operation = {};
		}
		bool Arm()
		{
			ResetEvent(operation.hEvent);
			return ReadDirectoryChangesW(file, buffer.data(), static_cast<DWORD>(buffer.size()), !parent,
				FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
				nullptr, &operation, nullptr) != FALSE;
		}
		bool Open()
		{
			retry = Clock::now() + s_retry;
			std::error_code error;
			directory = parent ? target.parent_path() : target;
			if (parent)
				while (!directory.empty() && !std::filesystem::is_directory(directory, error))
				{
					const auto next = directory.parent_path();
					if (next == directory) return false;
					directory = next;
				}
			// リンク先は監視しない。パックの読取り側も独立にパスを検証する。
			for (auto path = directory; !path.empty(); path = path.parent_path())
			{
				const auto attributes = GetFileAttributesW(path.c_str());
				if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
				if (path == path.root_path()) break;
			}
			file = CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
			if (file == INVALID_HANDLE_VALUE) return false;
			operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (!operation.hEvent || !Arm()) { Close(); return false; }
			return true;
		}
		bool Poll()
		{
			if (file == INVALID_HANDLE_VALUE) return Clock::now() >= retry && Open();
			if (WaitForSingleObject(operation.hEvent, 0) != WAIT_OBJECT_0) return false;
			DWORD bytes = 0;
			if (!GetOverlappedResult(file, &operation, &bytes, FALSE)) { Close(); retry = Clock::now() + s_retry; return true; }
			bool changed = bytes == 0;
			for (size_t offset = 0; bytes && offset < bytes;)
			{
				if (bytes - offset < offsetof(FILE_NOTIFY_INFORMATION, FileName)) { changed = true; break; }
				const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
				if (info->FileNameLength % sizeof(wchar_t) || info->FileNameLength > bytes - offset - offsetof(FILE_NOTIFY_INFORMATION, FileName)) { changed = true; break; }
				const auto path = directory / std::wstring(info->FileName, info->FileNameLength / sizeof(wchar_t));
				changed |= Related(path, target);
				if (!info->NextEntryOffset) break;
				if (info->NextEntryOffset < offsetof(FILE_NOTIFY_INFORMATION, FileName) || info->NextEntryOffset > bytes - offset) { changed = true; break; }
				offset += info->NextEntryOffset;
			}
			if (!Arm()) { Close(); retry = Clock::now() + s_retry; changed = true; }
			return changed;
		}
	};
} // namespace

struct DirectoryChangeMonitor::Impl
{
	std::vector<std::filesystem::path> paths;
	std::vector<std::unique_ptr<Watch>> watches;
	Clock::time_point inspectAt{};
	std::optional<uint64_t> signature;
	bool dirty = false;
	bool inspectionFailed = false;
	std::optional<uint64_t> Signature() const
	{
		uint64_t hash = 1469598103934665603ULL;
		size_t count = 0;
		const auto add = [&](const std::filesystem::path& path) {
			if (++count > 32768) throw std::runtime_error("monitor-entry-limit");
			WIN32_FILE_ATTRIBUTE_DATA info{};
			if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) throw std::runtime_error("monitor-file-busy");
			for (auto c : path.native()) hash = (hash ^ c) * 1099511628211ULL;
			for (auto value : {info.dwFileAttributes, info.ftLastWriteTime.dwLowDateTime, info.ftLastWriteTime.dwHighDateTime, info.nFileSizeHigh, info.nFileSizeLow}) hash = (hash ^ value) * 1099511628211ULL;
			return info.dwFileAttributes;
		};
		try
		{
			for (const auto& path : paths)
			{
				std::error_code error;
				if (!std::filesystem::exists(path, error)) { if (error) return {}; continue; }
				const auto attributes = add(path);
				if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
				for (std::filesystem::recursive_directory_iterator it(path), end; it != end; ++it)
					if (add(it->path()) & FILE_ATTRIBUTE_REPARSE_POINT) it.disable_recursion_pending();
			}
			return hash;
		}
		catch (...) { return {}; }
	}
};

DirectoryChangeMonitor::DirectoryChangeMonitor() : m_impl(std::make_unique<Impl>()) {}
DirectoryChangeMonitor::~DirectoryChangeMonitor() = default;
void DirectoryChangeMonitor::SetPaths(std::vector<std::filesystem::path> paths)
{
	auto& state = *m_impl;
	if (state.paths == paths) return;
	state.watches.clear(); state.paths = std::move(paths); state.signature.reset();
	state.dirty = !state.paths.empty(); state.inspectAt = Clock::now() + s_quiet;
	for (const auto& path : state.paths)
		for (bool parent : {false, true})
		{
			auto watch = std::make_unique<Watch>(); watch->target = path; watch->parent = parent;
			watch->Open(); state.watches.push_back(std::move(watch));
		}
}
bool DirectoryChangeMonitor::Poll(bool canInspect)
{
	auto& state = *m_impl;
	bool changed = false;
	for (const auto& watch : state.watches)
	{
		const bool notified = watch->Poll();
		changed |= notified;
		if (notified && watch->parent)
		{
			watch->Close(); watch->Open();
			for (const auto& child : state.watches)
				if (!child->parent && child->target == watch->target) { child->Close(); child->Open(); }
		}
	}
	if (changed) { state.dirty = true; state.signature.reset(); state.inspectAt = Clock::now() + s_quiet; }
	if (!state.dirty || !canInspect || Clock::now() < state.inspectAt) return false;
	const auto signature = state.Signature();
	state.inspectAt = Clock::now() + (signature ? s_quiet : s_retry);
	state.inspectionFailed = !signature;
	if (!signature) { state.signature.reset(); return false; }
	if (state.signature != signature) { state.signature = signature; return false; }
	state.signature.reset(); state.dirty = false; return true;
}
bool DirectoryChangeMonitor::IsAvailable() const
{
	if (m_impl->inspectionFailed) return false;
	for (const auto& watch : m_impl->watches)
		if (watch->file == INVALID_HANDLE_VALUE && (watch->parent || GetFileAttributesW(watch->target.c_str()) != INVALID_FILE_ATTRIBUTES)) return false;
	return true;
}
bool DirectoryChangeMonitor::HasPending() const { return m_impl->dirty; }
