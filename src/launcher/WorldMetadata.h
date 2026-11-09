#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace WorldMetadata
{

using Bytes = std::vector<uint8_t>;
struct Chunk
{
	size_t offset, size;
	std::string type;
};
struct Metadata
{
	std::string name;
	size_t nameUnits = 0, pngEnd = 0;
	std::vector<Chunk> chunks;
};

Metadata Parse(std::span<const uint8_t> bytes);
void ValidateName(std::string_view name);
Bytes Create(std::string_view name, std::span<const uint8_t> rgba);
Bytes Rename(std::span<const uint8_t> bytes, std::string_view name);
Bytes ReplaceImage(std::span<const uint8_t> bytes, std::span<const uint8_t> rgba);
Bytes Image(std::span<const uint8_t> bytes);
std::string Base64(std::span<const uint8_t> bytes);

} // namespace WorldMetadata
