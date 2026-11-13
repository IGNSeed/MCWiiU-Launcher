#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class TextureService final
{
public:
	struct OverlayFile
	{
		std::string virtualPath, sha256;
		std::filesystem::path path;
		size_t size;
	};
	TextureService(std::filesystem::path portable, std::filesystem::path baseCommon, std::filesystem::path updateCommon);
	~TextureService();
	bool IsHealthy() const;
	std::string Command(std::string_view payload, bool canEdit, bool canAcknowledge = false);
	std::string OpenFolder(const std::function<bool(const std::filesystem::path&)>& open, bool canAcknowledge = false);
	std::vector<OverlayFile> BeginTitle(uint64_t titleId, uint16_t version);
	void EndTitle();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
