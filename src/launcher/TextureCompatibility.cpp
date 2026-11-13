#include "TextureCompatibility.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace TextureCompatibility
{
namespace
{
	void Require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
	uint32_t Be(Bytes bytes, size_t offset, size_t count = 4)
	{
		Require(offset <= bytes.size() && count <= bytes.size() - offset, "invalid-binary");
		uint32_t result = 0;
		for (size_t i = 0; i < count; ++i) result = result << 8 | bytes[offset + i];
		return result;
	}
	uint32_t Le(Bytes bytes, size_t offset)
	{
		const auto value = Be(bytes, offset);
		return (value >> 24) | ((value >> 8) & 0xff00) | ((value << 8) & 0xff0000) | (value << 24);
	}
	bool Equal(Bytes a, Bytes b) { return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin()); }
	struct ArcEntry { std::string name; Bytes bytes; };
	std::vector<ArcEntry> Arc(Bytes bytes)
	{
		const auto count = Be(bytes, 0);
		Require(count == 508, "media-not-v560");
		std::vector<ArcEntry> result;
		std::vector<std::pair<size_t, size_t>> ranges;
		size_t cursor = 4;
		for (uint32_t i = 0; i < count; ++i)
		{
			const auto length = Be(bytes, cursor, 2); cursor += 2;
			Require(length && length <= 4096 && cursor + length + 8 <= bytes.size(), "invalid-media");
			std::string name(reinterpret_cast<const char*>(bytes.data() + cursor), length); cursor += length;
			Require(name.find('\0') == std::string::npos, "invalid-media");
			const auto start = Be(bytes, cursor), size = Be(bytes, cursor + 4); cursor += 8;
			Require(start <= bytes.size() && size <= bytes.size() - start, "invalid-media");
			result.push_back({std::move(name), bytes.subspan(start, size)}); ranges.emplace_back(start, size);
		}
		for (const auto& [offset, size] : ranges) { Require(offset == cursor, "invalid-media"); cursor += size; }
		Require(cursor == bytes.size(), "invalid-media"); return result;
	}
	void Hud(Bytes bytes, Bytes original)
	{
		// 実際の v560 の最終画像テーブルを使用し、prefix 内の PNG を画像テーブルと誤認しない。
		constexpr size_t count = 224, images = 102100, table = images - count * 32;
		Require(bytes.size() >= images && original.size() >= images && bytes[1] == 'I' && bytes[2] == 'U' && bytes[3] == 'F' && Le(bytes, 8) == bytes.size() - 152, "invalid-v560-hud");
		Require(Equal(bytes.first(8), original.first(8)) && Equal(bytes.subspan(12, table - 12), original.subspan(12, table - 12)), "media-hud-prefix-changed");
		size_t cursor = 0;
		for (size_t i = 0; i < count; ++i)
		{
			const auto d = table + i * 32;
			Require(Equal(bytes.subspan(d, 16), original.subspan(d, 16)) && Equal(bytes.subspan(d + 24, 8), original.subspan(d + 24, 8)), "media-hud-descriptor-changed");
			const auto offset = Le(bytes, d + 16), size = Le(bytes, d + 20);
			Require(offset == cursor && images + cursor <= bytes.size() && size <= bytes.size() - images - cursor, "invalid-v560-hud");
			const auto dimensions = Png(bytes.subspan(images + cursor, size), 4 * 1024 * 1024);
			Require(dimensions.first == Le(original, d + 8) && dimensions.second == Le(original, d + 12), "media-hud-image-size");
			cursor += size;
		}
		Require(images + cursor == bytes.size(), "invalid-v560-hud");
	}
	void PngStream(Bytes compressed, uint32_t width, uint32_t height, uint8_t depth, uint8_t color, uint8_t interlace)
	{
		const uint32_t channels = color == 0 || color == 3 ? 1 : color == 2 ? 3 : color == 4 ? 2 : color == 6 ? 4 : 0;
		Require(channels && (depth == 8 || (depth == 16 && color != 3) || ((depth == 1 || depth == 2 || depth == 4) && (color == 0 || color == 3))) && interlace <= 1, "invalid-png");
		std::vector<std::pair<size_t, uint32_t>> passes;
		auto pass = [&](uint32_t x, uint32_t y) { if (x && y) passes.emplace_back((uint64_t(x) * channels * depth + 7) / 8 + 1, y); };
		if (!interlace) pass(width, height);
		else
		{
			constexpr uint32_t startX[]{0, 4, 0, 2, 0, 1, 0}, startY[]{0, 0, 4, 0, 2, 0, 1};
			constexpr uint32_t stepX[]{8, 8, 4, 4, 2, 2, 1}, stepY[]{8, 8, 8, 4, 4, 2, 2};
			for (size_t i = 0; i < 7; ++i) pass(width > startX[i] ? (width - startX[i] + stepX[i] - 1) / stepX[i] : 0, height > startY[i] ? (height - startY[i] + stepY[i] - 1) / stepY[i] : 0);
		}
		size_t expected = 0;
		for (const auto& [stride, rows] : passes) expected += stride * rows;
		z_stream stream{};
		Require(inflateInit(&stream) == Z_OK, "invalid-png");
		struct End { z_stream* stream; ~End() { inflateEnd(stream); } } end{&stream};
		stream.next_in = const_cast<Bytef*>(compressed.data()); stream.avail_in = static_cast<uInt>(compressed.size());
		std::array<uint8_t, 64 * 1024> buffer{};
		size_t total = 0, nextFilter = 0, passIndex = 0; uint32_t row = 0;
		int status;
		do
		{
			stream.next_out = buffer.data(); stream.avail_out = static_cast<uInt>(buffer.size());
			status = inflate(&stream, Z_NO_FLUSH);
			Require(status == Z_OK || status == Z_STREAM_END, "invalid-png");
			const auto produced = buffer.size() - stream.avail_out;
			Require((produced || status == Z_STREAM_END) && total + produced <= expected, "invalid-png");
			while (nextFilter < total + produced)
			{
				Require(passIndex < passes.size() && buffer[nextFilter - total] <= 4, "invalid-png");
				nextFilter += passes[passIndex].first;
				if (++row == passes[passIndex].second) { ++passIndex; row = 0; }
			}
			total += produced;
		} while (status != Z_STREAM_END);
		Require(total == expected && !stream.avail_in && passIndex == passes.size(), "invalid-png");
	}
} // namespace

std::pair<uint32_t, uint32_t> Png(Bytes bytes, uint64_t pixelLimit)
{
	constexpr std::array<uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
	Require(bytes.size() >= 45 && std::equal(signature.begin(), signature.end(), bytes.begin()) && Be(bytes, 8) == 13 && !memcmp(bytes.data() + 12, "IHDR", 4), "invalid-png");
	const auto width = Be(bytes, 16), height = Be(bytes, 20);
	Require(width && height && width <= 8192 && height <= 8192 && uint64_t(width) * height <= pixelLimit, "png-size-limit");
	Require(bytes[26] == 0 && bytes[27] == 0, "invalid-png");
	bool end = false;
	std::vector<uint8_t> compressed;
	for (size_t offset = 8; offset < bytes.size();)
	{
		const auto size = Be(bytes, offset);
		Require(bytes.size() - offset >= 12 && size <= bytes.size() - offset - 12, "invalid-png");
		Require(crc32(0, bytes.data() + offset + 4, size + 4) == Be(bytes, offset + 8 + size), "png-crc");
		if (!memcmp(bytes.data() + offset + 4, "IDAT", 4)) compressed.insert(compressed.end(), bytes.begin() + offset + 8, bytes.begin() + offset + 8 + size);
		if (!memcmp(bytes.data() + offset + 4, "IEND", 4)) { Require(!size && offset + 12 == bytes.size(), "invalid-png"); end = true; }
		offset += size + 12;
	}
	Require(end && !compressed.empty(), "invalid-png");
	// WIC が欠損した画素を補完しても、圧縮ストリームの破損や不足は受け入れない。
	PngStream(compressed, width, height, bytes[24], bytes[25], bytes[28]);
	using Microsoft::WRL::ComPtr;
	ComPtr<IWICImagingFactory> factory; ComPtr<IWICStream> stream; ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> frame; ComPtr<IWICFormatConverter> converter;
	Require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
		SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) &&
		SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) && SUCCEEDED(decoder->GetFrame(0, &frame)) &&
		SUCCEEDED(factory->CreateFormatConverter(&converter)) && SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)), "invalid-png");
	UINT decodedWidth = 0, decodedHeight = 0;
	Require(SUCCEEDED(frame->GetSize(&decodedWidth, &decodedHeight)) && decodedWidth == width && decodedHeight == height, "invalid-png");
	std::vector<uint8_t> decoded(static_cast<size_t>(width) * height * 4);
	Require(SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(decoded.size()), decoded.data())), "invalid-png");
	return {width, height};
}

void Media(Bytes bytes, Bytes vanilla)
{
	const auto entries = Arc(bytes), reference = Arc(vanilla);
	for (size_t i = 0; i < entries.size(); ++i)
	{
		Require(entries[i].name == reference[i].name, "media-entry-mismatch");
		if (Equal(entries[i].bytes, reference[i].bytes)) continue;
		Require(entries[i].name == "skinGraphicsHud.fui", "media-unverified-entry");
		Hud(entries[i].bytes, reference[i].bytes);
	}
}

std::vector<uint8_t> Icon(Bytes bytes)
{
	const auto [width, height] = Png(bytes, 1024 * 1024);
	const auto longest = std::max(width, height);
	const auto targetWidth = std::max(1U, width * std::min(longest, 80U) / longest), targetHeight = std::max(1U, height * std::min(longest, 80U) / longest);
	using Microsoft::WRL::ComPtr;
	ComPtr<IWICImagingFactory> factory; ComPtr<IWICStream> input; ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> source; ComPtr<IWICBitmapScaler> scaler;
	ComPtr<IStream> output; ComPtr<IWICBitmapEncoder> encoder; ComPtr<IWICBitmapFrameEncode> frame;
	WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
	Require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
		SUCCEEDED(factory->CreateStream(&input)) && SUCCEEDED(input->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) &&
		SUCCEEDED(factory->CreateDecoderFromStream(input.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) && SUCCEEDED(decoder->GetFrame(0, &source)) &&
		SUCCEEDED(factory->CreateBitmapScaler(&scaler)) && SUCCEEDED(scaler->Initialize(source.Get(), targetWidth, targetHeight, WICBitmapInterpolationModeFant)) &&
		SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &output)) && SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
		SUCCEEDED(encoder->Initialize(output.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
		SUCCEEDED(frame->SetSize(targetWidth, targetHeight)) && SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WriteSource(scaler.Get(), nullptr)) &&
		SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()), "invalid-icon");
	STATSTG info{}; Require(SUCCEEDED(output->Stat(&info, STATFLAG_NONAME)) && info.cbSize.QuadPart <= 64 * 1024, "icon-size-limit");
	std::vector<uint8_t> result(static_cast<size_t>(info.cbSize.QuadPart)); LARGE_INTEGER start{}; ULONG read = 0;
	Require(SUCCEEDED(output->Seek(start, STREAM_SEEK_SET, nullptr)) && SUCCEEDED(output->Read(result.data(), static_cast<ULONG>(result.size()), &read)) && read == result.size(), "invalid-icon");
	return result;
}

bool Animation(std::string_view path)
{
	constexpr std::string_view root = "Common/res/TitleUpdate/res/textures/blocks/";
	if (!path.starts_with(root)) return false;
	const auto name = path.substr(root.size());
	return name == "fire_0.png" || name == "fire_1.png" || name == "lava.png" || name == "lava_flow.png" ||
		name == "water.png" || name == "water_flow.png" || name == "portal.png" || name == "prismarine_rough.png" || name == "sea_lantern.png";
}

void Sequence(Bytes bytes, Bytes image)
{
	Require(bytes.size() <= 16384, "animation-sequence-limit");
	const auto [width, height] = Png(image);
	Require(height >= width && height % width == 0, "animation-strip-size");
	const auto frames = height / width;
	const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	size_t offset = 0, entries = 0;
	uint64_t ticks = 0;
	auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
	auto number = [&]() {
		uint32_t value = 0;
		const auto start = text.data() + offset;
		const auto parsed = std::from_chars(start, text.data() + text.size(), value);
		Require(parsed.ec == std::errc{} && parsed.ptr != start, "invalid-animation-sequence");
		offset = static_cast<size_t>(parsed.ptr - text.data()); return value;
	};
	while (offset < text.size())
	{
		while (offset < text.size() && space(text[offset])) ++offset;
		if (offset == text.size()) break;
		const auto frame = number();
		Require(frame < frames, "animation-frame-range");
		uint32_t duration = 1;
		if (offset < text.size() && text[offset] == '*') { ++offset; duration = number(); }
		Require(duration && ++entries <= 4096 && (ticks += duration) <= 65536, "animation-sequence-limit");
		Require(offset == text.size() || text[offset] == ',' || space(text[offset]), "invalid-animation-sequence");
		if (offset < text.size() && text[offset] == ',') ++offset;
	}
}

std::string Resource(std::string_view path, Bytes bytes, Bytes vanilla)
{
	if (path == "Common/Media/MediaWiiU.arc") { Media(bytes, vanilla); return {}; }
	if (path == "Common/res/misc/sky.png")
	{
		const auto [width, height] = Png(bytes);
		Require(width % 3 == 0 && height % 2 == 0 && width / 3 == height / 2, "incompatible-sky");
		return width == 4032 && height == 2688 ? "" : "unusual-png-size";
	}
	Require(path.ends_with(".png") && !vanilla.empty(), "unsupported-resource");
	const auto [width, height] = Png(bytes);
	const auto [baseWidth, baseHeight] = Png(vanilla);
	if (Animation(path))
	{
		// 縦の長さはフレーム数であり、Default と同じ縦横比である必要はない。
		Require(height >= width && height % width == 0, "animation-strip-size");
		return width == baseWidth && height == baseHeight ? "" : "animation-layout";
	}
	if (path == "Common/res/TitleUpdate/res/items.png")
	{
		Require(baseWidth == 256 && baseHeight == 272, "vanilla-not-v560");
		Require(width % 16 == 0 && height % 17 == 0 && width / 16 == height / 17, "incompatible-items");
	}
	if (path == "Common/res/TitleUpdate/res/terrain.png" || path == "Common/res/TitleUpdate/res/terrainMipMapLevel2.png" || path == "Common/res/TitleUpdate/res/terrainMipMapLevel3.png")
		Require(uint64_t(height) == uint64_t(width) * 2, "incompatible-terrain");
	if (path == "Common/res/TitleUpdate/res/particles.png")
	{
		Require(baseWidth == 128 && baseHeight == 128, "vanilla-not-v560");
		Require(width == height, "incompatible-particles");
	}
	// 倍率の目安はファイルの安全性とは分離し、確認画面で注意を表示する。
	const auto scale = width / baseWidth;
	const bool usual = width >= baseWidth && width % baseWidth == 0 && uint64_t(height) * baseWidth == uint64_t(baseHeight) * width && scale <= 16 && !(scale & (scale - 1));
	return usual && (path != "Common/res/TitleUpdate/res/terrain.png" || scale <= 2) ? "" : "unusual-png-size";
}
} // namespace TextureCompatibility
