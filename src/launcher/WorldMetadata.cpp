#include "WorldMetadata.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <zlib.h>

namespace WorldMetadata
{
namespace
{
	constexpr std::array<uint8_t, 8> s_signature{137, 80, 78, 71, 13, 10, 26, 10};
	void Require(bool ok) { if (!ok) throw std::runtime_error("unsupported-metadata"); }
	uint32_t Read32(std::span<const uint8_t> b, size_t offset)
	{
		Require(offset <= b.size() && b.size() - offset >= 4);
		return uint32_t(b[offset]) << 24 | uint32_t(b[offset + 1]) << 16 | uint32_t(b[offset + 2]) << 8 | b[offset + 3];
	}
	void Append32(Bytes& b, uint32_t value)
	{
		for (int shift = 24; shift >= 0; shift -= 8) b.push_back(static_cast<uint8_t>(value >> shift));
	}
	void AppendChunk(Bytes& b, std::string_view type, std::span<const uint8_t> data)
	{
		Append32(b, static_cast<uint32_t>(data.size()));
		const auto begin = b.size();
		b.insert(b.end(), type.begin(), type.end()); b.insert(b.end(), data.begin(), data.end());
		Append32(b, crc32(0, b.data() + begin, static_cast<uInt>(4 + data.size())));
	}
	std::wstring Wide(std::string_view name)
	{
		if (name.empty() || name.size() > 512 || name.find('\0') != std::string_view::npos) throw std::runtime_error("invalid-name");
		const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0);
		if (length <= 0 || length > 32) throw std::runtime_error("invalid-name");
		std::wstring wide(length, 0);
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), wide.data(), length);
		if (std::any_of(wide.begin(), wide.end(), [](wchar_t c) { return c < 32 || c == 127; }) ||
			std::all_of(wide.begin(), wide.end(), [](wchar_t c) { return c == L' ' || c == 0x3000; })) throw std::runtime_error("invalid-name");
		return wide;
	}
}

Metadata Parse(std::span<const uint8_t> bytes)
{
	Require(bytes.size() >= 256 + 57 && bytes.size() <= 1024 * 1024);
	Metadata result;
	std::wstring wide;
	// 先頭 0x100 bytes は UTF-16BE の名前バッファ。終端後の内容は解釈しない。
	for (size_t i = 0; i < 128; ++i)
	{
		const auto unit = static_cast<wchar_t>(uint16_t(bytes[i * 2]) << 8 | bytes[i * 2 + 1]);
		if (!unit) { result.nameUnits = i; break; }
		Require(i < 127); wide.push_back(unit);
	}
	Require(!wide.empty());
	const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
	Require(length > 0);
	result.name.resize(length);
	WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), result.name.data(), length, nullptr, nullptr);
	Require(std::equal(s_signature.begin(), s_signature.end(), bytes.begin() + 256));
	size_t offset = 264;
	bool header = false, data = false, dataEnded = false, ended = false;
	Bytes compressed;
	while (offset + 12 <= bytes.size())
	{
		const size_t size = Read32(bytes, offset);
		Require(size <= bytes.size() - offset - 12);
		const std::string type(reinterpret_cast<const char*>(bytes.data() + offset + 4), 4);
		Require(std::all_of(type.begin(), type.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }));
		Require(crc32(0, bytes.data() + offset + 4, static_cast<uInt>(size + 4)) == Read32(bytes, offset + size + 8));
		if (!header)
		{
			Require(type == "IHDR" && size == 13 && Read32(bytes, offset + 8) == 64 && Read32(bytes, offset + 12) == 64);
			Require(bytes[offset + 16] == 8 && bytes[offset + 17] == 6 && bytes[offset + 18] == 0 && bytes[offset + 19] == 0 && bytes[offset + 20] == 0);
			header = true;
		}
		else if (type == "IDAT")
		{
			Require(!dataEnded && size > 0); data = true;
			compressed.insert(compressed.end(), bytes.begin() + offset + 8, bytes.begin() + offset + 8 + size);
		}
		else
		{
			if (data) dataEnded = true;
			Require(type != "IHDR" && type != "PLTE" && type != "tRNS" && type != "acTL" && type != "fcTL" && type != "fdAT");
			Require(type == "IEND" || (type[0] >= 'a' && type[0] <= 'z'));
		}
		result.chunks.push_back({offset, size + 12, type}); offset += size + 12;
		if (type == "IEND") { Require(size == 0 && data); ended = true; break; }
	}
	Require(ended);
	// 圧縮データも固定寸法まで展開し、壊れた PNG や展開爆弾を拒否する。
	std::array<uint8_t, 64 * (1 + 64 * 4)> decoded{};
	z_stream stream{};
	Require(inflateInit(&stream) == Z_OK);
	stream.next_in = compressed.data(); stream.avail_in = static_cast<uInt>(compressed.size());
	stream.next_out = decoded.data(); stream.avail_out = static_cast<uInt>(decoded.size());
	const auto status = inflate(&stream, Z_FINISH);
	const bool valid = status == Z_STREAM_END && stream.total_out == decoded.size() && stream.avail_in == 0;
	inflateEnd(&stream); Require(valid);
	for (size_t row = 0; row < 64; ++row) Require(decoded[row * 257] <= 4);
	result.pngEnd = offset;
	return result;
}

void ValidateName(std::string_view name)
{
	Wide(name);
}

Bytes Create(std::string_view name, std::span<const uint8_t> rgba)
{
	const auto wide = Wide(name); Require(rgba.size() == 64 * 64 * 4);
	// 確認済みの名前バッファと PNG だけを新規生成し、他のワールドの領域を持ち込まない。
	Bytes result(256, 0);
	for (size_t i = 0; i < wide.size(); ++i)
	{
		result[i * 2] = static_cast<uint8_t>(wide[i] >> 8); result[i * 2 + 1] = static_cast<uint8_t>(wide[i]);
	}
	result.insert(result.end(), s_signature.begin(), s_signature.end());
	Bytes header; Append32(header, 64); Append32(header, 64); header.insert(header.end(), {8, 6, 0, 0, 0}); AppendChunk(result, "IHDR", header);
	Bytes raw; raw.reserve(64 * 257);
	for (size_t row = 0; row < 64; ++row) { raw.push_back(0); raw.insert(raw.end(), rgba.begin() + row * 256, rgba.begin() + (row + 1) * 256); }
	Bytes compressed(compressBound(static_cast<uLong>(raw.size()))); uLongf size = static_cast<uLongf>(compressed.size());
	Require(compress2(compressed.data(), &size, raw.data(), static_cast<uLong>(raw.size()), Z_BEST_COMPRESSION) == Z_OK); compressed.resize(size);
	AppendChunk(result, "IDAT", compressed); AppendChunk(result, "IEND", {});
	const auto metadata = Parse(result); Require(metadata.name == name && metadata.pngEnd == result.size()); return result;
}

Bytes Rename(std::span<const uint8_t> bytes, std::string_view name)
{
	const auto before = Parse(bytes);
	const auto wide = Wide(name);
	Bytes result(bytes.begin(), bytes.end());
	for (size_t i = 0; i <= wide.size(); ++i)
	{
		const uint16_t unit = i < wide.size() ? wide[i] : 0;
		result[i * 2] = static_cast<uint8_t>(unit >> 8); result[i * 2 + 1] = static_cast<uint8_t>(unit);
	}
	Require(Parse(result).name == name);
	return result;
}

Bytes ReplaceImage(std::span<const uint8_t> bytes, std::span<const uint8_t> rgba)
{
	const auto before = Parse(bytes);
	// PNG 後の未検証領域や、画像内容に依存し得る未知チャンクは移動しない。
	Require(before.pngEnd == bytes.size());
	for (const auto& chunk : before.chunks)
		Require(chunk.type == "IHDR" || chunk.type == "IDAT" || chunk.type == "IEND" || chunk.type == "sRGB" || chunk.type == "tEXt" || (chunk.type[3] >= 'a' && chunk.type[3] <= 'z'));
	Require(rgba.size() == 64 * 64 * 4);
	Bytes raw;
	for (size_t row = 0; row < 64; ++row)
	{
		raw.push_back(0); raw.insert(raw.end(), rgba.begin() + row * 256, rgba.begin() + (row + 1) * 256);
	}
	Bytes compressed(compressBound(static_cast<uLong>(raw.size())));
	uLongf size = static_cast<uLongf>(compressed.size());
	Require(compress2(compressed.data(), &size, raw.data(), static_cast<uLong>(raw.size()), Z_BEST_COMPRESSION) == Z_OK);
	compressed.resize(size);
	Bytes result(bytes.begin(), bytes.begin() + 264);
	bool inserted = false;
	for (const auto& chunk : before.chunks)
	{
		if (chunk.type == "IDAT")
		{
			if (!inserted) { AppendChunk(result, "IDAT", compressed); inserted = true; }
		}
		else result.insert(result.end(), bytes.begin() + chunk.offset, bytes.begin() + chunk.offset + chunk.size);
	}
	result.insert(result.end(), bytes.begin() + before.pngEnd, bytes.end());
	Require(Parse(result).name == before.name);
	return result;
}

Bytes Image(std::span<const uint8_t> bytes)
{
	const auto metadata = Parse(bytes);
	return {bytes.begin() + 256, bytes.begin() + metadata.pngEnd};
}

std::string Base64(std::span<const uint8_t> bytes)
{
	constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string result;
	for (size_t i = 0; i < bytes.size(); i += 3)
	{
		const uint32_t word = uint32_t(bytes[i]) << 16 | (i + 1 < bytes.size() ? uint32_t(bytes[i + 1]) << 8 : 0) | (i + 2 < bytes.size() ? bytes[i + 2] : 0);
		result += alphabet[word >> 18]; result += alphabet[(word >> 12) & 63];
		result += i + 1 < bytes.size() ? alphabet[(word >> 6) & 63] : '=';
		result += i + 2 < bytes.size() ? alphabet[word & 63] : '=';
	}
	return result;
}

} // namespace WorldMetadata
