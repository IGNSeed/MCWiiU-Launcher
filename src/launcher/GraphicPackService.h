#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Host の workMutex で設定変更とタイトルの準備・終了を直列化する。
class GraphicPackService final
{
public:
	GraphicPackService();
	~GraphicPackService();
	bool IsHealthy() const;
	bool Reload();
	std::string Command(std::string_view payload, bool canEdit);
	std::string OpenFolder(const std::function<bool(const std::filesystem::path&)>& open);
	void PrepareLaunch(int vsyncFrequency, unsigned width, unsigned height, uint64_t titleId = 0x00050000101dbe00ULL);
	void ValidateReplacements(const std::vector<std::string>& texturePaths) const;
	void EndTitle();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
