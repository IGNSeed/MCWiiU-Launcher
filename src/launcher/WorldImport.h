#pragma once

#include "WorldMetadata.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

// Native picker が許可した入力だけを保持する、読み取り専用のプレビュー。
class WorldImport final
{
public:
	using Reader = std::function<WorldMetadata::Bytes(const std::filesystem::path&, size_t)>;
	struct Candidate
	{
		std::string id, account, name, icon, bodyHash, extHash, worldFile, metadataFile, imageFile, problem;
		uint64_t size = 0;
		bool ready = false, generated = true;
	};
	struct Data
	{
		WorldMetadata::Bytes body, ext;
		std::filesystem::file_time_type bodyTime, extTime;
	};
	explicit WorldImport(Reader reader);
	~WorldImport();
	void SelectBody(const std::filesystem::path& selected);
	void SelectMetadata(const std::filesystem::path& selected);
	void ClearMetadata();
	void SetName(std::string name);
	void ClearIcon();
	void SetIcon(WorldMetadata::Bytes rgba, std::string displayName);
	const Candidate& Preview() const;
	Data ReadVerified() const;
	static void ValidateBody(std::span<const uint8_t> body);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
