#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TextureCompatibility
{
using Bytes = std::span<const uint8_t>;
std::pair<uint32_t, uint32_t> Png(Bytes bytes, uint64_t pixelLimit = 32 * 1024 * 1024);
std::vector<uint8_t> Icon(Bytes bytes);
bool Animation(std::string_view path);
void Sequence(Bytes bytes, Bytes image);
std::string Resource(std::string_view path, Bytes bytes, Bytes vanilla);
void Media(Bytes bytes, Bytes vanilla);
} // namespace TextureCompatibility
