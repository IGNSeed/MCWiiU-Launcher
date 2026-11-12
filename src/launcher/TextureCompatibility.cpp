#include "TextureCompatibility.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <zlib.h>
#include <algorithm>
#include <array>
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
} // namespace

std::pair<uint32_t, uint32_t> Png(Bytes bytes, uint64_t pixelLimit)
{
	constexpr std::array<uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
	Require(bytes.size() >= 45 && std::equal(signature.begin(), signature.end(), bytes.begin()) && Be(bytes, 8) == 13 && !memcmp(bytes.data() + 12, "IHDR", 4), "invalid-png");
	const auto width = Be(bytes, 16), height = Be(bytes, 20);
	Require(width && height && width <= 8192 && height <= 8192 && uint64_t(width) * height <= pixelLimit, "png-size-limit");
	bool end = false, data = false;
	for (size_t offset = 8; offset < bytes.size();)
	{
		const auto size = Be(bytes, offset);
		Require(bytes.size() - offset >= 12 && size <= bytes.size() - offset - 12, "invalid-png");
		Require(crc32(0, bytes.data() + offset + 4, size + 4) == Be(bytes, offset + 8 + size), "png-crc");
		if (!memcmp(bytes.data() + offset + 4, "IDAT", 4)) data = true;
		if (!memcmp(bytes.data() + offset + 4, "IEND", 4)) { Require(!size && offset + 12 == bytes.size(), "invalid-png"); end = true; }
		offset += size + 12;
	}
	Require(end && data, "invalid-png");
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

void Resource(std::string_view path, Bytes bytes, Bytes vanilla)
{
	if (path == "Common/Media/MediaWiiU.arc") { Media(bytes, vanilla); return; }
	if (path == "Common/res/misc/sky.png")
	{
		Require(Png(bytes) == std::pair<uint32_t, uint32_t>{4032, 2688}, "incompatible-sky"); return;
	}
	Require(path.ends_with(".png") && !vanilla.empty(), "unsupported-resource");
	const auto [width, height] = Png(bytes);
	const auto [baseWidth, baseHeight] = Png(vanilla);
	Require(width >= baseWidth && width % baseWidth == 0 && uint64_t(height) * baseWidth == uint64_t(baseHeight) * width, "incompatible-png-size");
	const auto scale = width / baseWidth;
	Require(scale <= 16 && !(scale & (scale - 1)), "incompatible-png-size");
	if (path == "Common/res/TitleUpdate/res/items.png") Require(baseWidth == 256 && baseHeight == 272, "vanilla-not-v560");
	if (path == "Common/res/TitleUpdate/res/terrain.png") Require(baseWidth == 256 && baseHeight == 512 && scale <= 2, "incompatible-terrain");
	if (path == "Common/res/TitleUpdate/res/particles.png") Require(baseWidth == 128 && baseHeight == 128, "vanilla-not-v560");
}
} // namespace TextureCompatibility
