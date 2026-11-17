#pragma once

#include <Windows.h>
#include <filesystem>
#include <string_view>
#include <vector>

// 開発版と配布版で共有する保存先。EXE、環境変数、旧 runtime は参照しない。
class LauncherPaths final
{
public:
	static LauncherPaths& Get();
	static std::filesystem::path LocalRoot();
	explicit LauncherPaths(std::filesystem::path root);
	~LauncherPaths();
	LauncherPaths(const LauncherPaths&) = delete;
	LauncherPaths& operator=(const LauncherPaths&) = delete;
	void Initialize();
	void ValidateTree() const;
	void CheckPath(const std::filesystem::path& path) const;
	bool ContainsData(const std::filesystem::path& path) const;
	void CreateDefault(const std::filesystem::path& path, std::string_view bytes) const;
	const std::filesystem::path root, data, cache, logs, webview;

private:
	std::vector<HANDLE> m_directories;
	bool m_initialized = false;
};
