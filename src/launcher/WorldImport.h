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
	enum class Kind { Files, Folder, Zip };
	using Reader = std::function<WorldMetadata::Bytes(const std::filesystem::path&, size_t)>;
	struct Candidate
	{
		std::string key, id, account, name, icon, problem, bodyHash, extHash;
		uint64_t size = 0;
	};
	struct Data
	{
		WorldMetadata::Bytes body, ext;
		std::filesystem::file_time_type bodyTime, extTime;
	};
	WorldImport(std::filesystem::path selected, Kind kind, Reader reader);
	~WorldImport();
	const std::vector<Candidate>& Candidates() const;
	Data ReadCandidate(std::string_view key) const;
	static void ValidateBody(std::span<const uint8_t> body);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
