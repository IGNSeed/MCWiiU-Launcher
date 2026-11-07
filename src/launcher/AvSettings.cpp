#include "AvSettings.h"

#include "config/ActiveSettings.h"
#include "audio/IAudioAPI.h"
#include "audio/IAudioInputAPI.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <pugixml.hpp>
#include <Windows.h>
#include <fstream>

namespace
{
	using Writer = rapidjson::Writer<rapidjson::StringBuffer>;
	void RequireAv(bool valid) { if (!valid) throw std::runtime_error("invalid-av-setting"); }
	void CheckAvPath(fs::path path)
	{
		for (; !path.empty(); path = path.parent_path())
		{
			const auto attributes = GetFileAttributesW(path.c_str());
			RequireAv(attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT));
			if (path == path.root_path()) break;
		}
	}
	std::string ReadAv(const fs::path& path)
	{
		CheckAvPath(path);
		RequireAv(fs::file_size(path) <= 4 * 1024 * 1024);
		std::ifstream stream(path, std::ios::binary);
		RequireAv(stream.good());
		return {std::istreambuf_iterator<char>(stream), {}};
	}
	void WriteAv(const fs::path& path, std::string_view bytes)
	{
		CheckAvPath(path);
		const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		RequireAv(handle != INVALID_HANDLE_VALUE);
		DWORD written = 0;
		const bool ok = WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(handle);
		const bool closed = CloseHandle(handle) != FALSE;
		if (!ok || !closed) { DeleteFileW(path.c_str()); RequireAv(false); }
	}
	void SetAv(pugi::xml_node parent, const char* key, const std::string& value)
	{
		auto child = parent.child(key);
		if (!child) child = parent.append_child(key);
		child.text().set(value.c_str());
	}
	std::string UuidText(const std::array<uint8, 16>& uuid)
	{
		std::string value;
		for (auto byte : uuid) value += fmt::format("{:02x}", byte);
		return value;
	}
} // namespace

struct AvSettings::Impl
{
	struct Device
	{
		uint64 token;
		std::wstring identifier;
		std::string name;
	};
	std::vector<Device> output, input, gpu;
	std::vector<int> apis;
	uint64 revision = 1, sequence = 0, tokenCounter = 0;
	std::string notice = "ready", enumerationNotice;
	std::chrono::steady_clock::time_point nextRefresh{};
	fs::path path = ActiveSettings::GetConfigPath("settings.xml");
	int enumeratedApi = -1;
	std::wstring channelDevice;
	int channelApi = -1;
	std::vector<int> channels;
	LatteOverlaySettings overlay{GetConfig().overlay, GetConfig().notification, 1};
	uint64 appliedRevision = 1;
	std::string applyNotice = "ready";

	template<typename Descriptions>
	void UpdateDevices(std::vector<Device>& target, const Descriptions& descriptions)
	{
		std::vector<Device> fresh;
		for (const auto& description : descriptions)
		{
			const auto identifier = description->GetIdentifier();
			const auto old = std::find_if(target.begin(), target.end(), [&](const auto& device) { return device.identifier == identifier; });
			fresh.push_back({old == target.end() ? ++tokenCounter : old->token, identifier, identifier == L"default" ? "Default" : boost::nowide::narrow(description->GetName())});
		}
		if (&target == &output && (target.size() != fresh.size() || !std::equal(target.begin(), target.end(), fresh.begin(), [](const auto& a, const auto& b) { return a.identifier == b.identifier; }))) channelApi = -1;
		target = std::move(fresh);
	}
	void Refresh(bool force = false)
	{
		const auto& config = GetConfig();
		if (!force && enumeratedApi == config.audio_api && std::chrono::steady_clock::now() < nextRefresh) return;
		nextRefresh = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		enumerationNotice.clear();
		try
		{
			apis.clear();
			for (int api = 0; api < IAudioAPI::AudioAPIEnd; ++api)
				if (IAudioAPI::IsAudioAPIAvailable(static_cast<IAudioAPI::AudioAPI>(api))) apis.push_back(api);
			if (enumeratedApi != config.audio_api) output.clear();
			enumeratedApi = config.audio_api;
			if (std::find(apis.begin(), apis.end(), config.audio_api) != apis.end())
				UpdateDevices(output, IAudioAPI::GetDevices(static_cast<IAudioAPI::AudioAPI>(config.audio_api), true));
			else output.clear();
			if (IAudioInputAPI::IsAudioInputAPIAvailable(IAudioInputAPI::Cubeb)) UpdateDevices(input, IAudioInputAPI::GetDevices(IAudioInputAPI::Cubeb));
			else input.clear();
		}
		catch (...) { output.clear(); input.clear(); enumerationNotice = "device-enumeration-failed"; }
	}
	void RefreshChannels()
	{
		const auto& config = GetConfig();
		if (channelApi == config.audio_api && channelDevice == config.tv_device) return;
		channelApi = config.audio_api; channelDevice = config.tv_device; channels.clear();
		if (config.tv_device.empty()) { channels = {kMono, kStereo, kSurround}; return; }
		try
		{
			for (const auto& device : IAudioAPI::GetDevices(static_cast<IAudioAPI::AudioAPI>(config.audio_api)))
				if (device->GetIdentifier() == config.tv_device)
					for (int channel = kMono; channel <= kSurround; ++channel)
					{
						try
						{
							// 列挙だけでは分からない形式は、再生しない出力 Stream で確認する。
							const auto probe = IAudioAPI::CreateDevice(static_cast<IAudioAPI::AudioAPI>(config.audio_api), device, 48000, CemuConfig::AudioChannelsToNChannels(static_cast<AudioChannels>(channel)), 576, 16);
							if (probe) channels.push_back(channel);
						}
						catch (...) {}
					}
		}
		catch (...) {}
	}
	void Persist(pugi::xml_document& document)
	{
		const auto original = ReadAv(path);
		std::ostringstream stream; document.save(stream);
		const auto bytes = stream.str();
		const auto suffix = fmt::format("{}-{}-{}", GetCurrentProcessId(), GetTickCount64(), ++sequence);
		const auto temporary = fs::path(path.wstring() + L".pending-" + _utf8ToPath(suffix).wstring());
		const auto backups = path.parent_path() / ".launcher-av-backups";
		CheckAvPath(backups); fs::create_directories(backups);
		bool replaced = false;
		try
		{
			WriteAv(backups / (suffix + ".xml"), original);
			WriteAv(temporary, bytes);
			CemuConfig candidate;
			XMLConfig<CemuConfig> validation(candidate, temporary.wstring());
			RequireAv(validation.Load() && ReadAv(temporary) == bytes);
			RequireAv(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
			replaced = true;
			pugi::xml_document verified;
			RequireAv(ReadAv(path) == bytes && verified.load_file(path.c_str()));
			++revision; notice = "saved";
		}
		catch (...)
		{
			DeleteFileW(temporary.c_str());
			bool restored = !replaced;
			if (replaced)
			{
				const auto rollback = fs::path(path.wstring() + L".rollback-" + _utf8ToPath(suffix).wstring());
				try { WriteAv(rollback, original); restored = MoveFileExW(rollback.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) && ReadAv(path) == original; }
				catch (...) { restored = false; }
			}
			notice = restored ? "save-failed" : "recovery-required";
			throw;
		}
	}
	static int Token(const std::vector<Device>& devices, const std::wstring& identifier)
	{
		if (identifier.empty()) return 0;
		const auto it = std::find_if(devices.begin(), devices.end(), [&](const auto& device) { return device.identifier == identifier; });
		return it == devices.end() ? -1 : static_cast<int>(it->token);
	}
};

AvSettings::AvSettings() : m_impl(std::make_unique<Impl>())
{
	try
	{
		for (const auto& device : VulkanRenderer::GetDevices())
			m_impl->gpu.push_back({++m_impl->tokenCounter, boost::nowide::widen(UuidText(device.uuid)), device.name});
	}
	catch (...) {}
	// Launcher では保存済み OFF も移行し、通常 Cemu の既定値は変えない。
	GetConfig().play_boot_sound = true;
	try
	{
		if (fs::exists(m_impl->path))
		{
			const auto original = ReadAv(m_impl->path);
			pugi::xml_document stored;
			RequireAv(stored.load_buffer(original.data(), original.size()));
			auto root = stored.child("content"); RequireAv(bool(root));
			if (!root.child("play_boot_sound").text().as_bool())
			{
				SetAv(root, "play_boot_sound", "true");
				m_impl->Persist(stored);
			}
		}
	}
	catch (...) { if (m_impl->notice != "recovery-required") m_impl->notice = "save-failed"; }
	LatteOverlay_setSettings(m_impl->overlay);
	m_impl->Refresh();
	m_impl->RefreshChannels();
}
AvSettings::~AvSettings() = default;
bool AvSettings::IsHealthy() const { return m_impl->notice != "recovery-required"; }

std::string AvSettings::Snapshot()
{
	auto& s = *m_impl; s.Refresh();
	const auto& c = GetConfig();
	rapidjson::StringBuffer buffer; Writer w(buffer); w.StartObject();
	w.Key("revision"); w.Uint64(s.revision); w.Key("notice"); w.String(s.notice.c_str());
	w.Key("enumerationNotice"); w.String(s.enumerationNotice.c_str());
	w.Key("overlayPublishedRevision"); w.Uint64(s.overlay.revision);
	w.Key("overlayRenderedRevision"); w.Uint64(LatteOverlay_getRenderedRevision());
	w.Key("appliedRevision"); w.Uint64(s.appliedRevision); w.Key("applyNotice"); w.String(s.applyNotice.c_str());
	w.Key("values"); w.StartObject();
	w.Key("graphicsApi"); w.Int(c.graphic_api);
	const auto uuid = UuidText(c.vk_graphic_device_uuid);
	w.Key("gpu"); w.Int(uuid == std::string(32, '0') ? 0 : Impl::Token(s.gpu, boost::nowide::widen(uuid)));
	w.Key("vsync"); w.Int(c.vsync); w.Key("async"); w.Bool(c.async_compile); w.Key("fullSync"); w.Bool(c.gx2drawdone_sync);
	w.Key("upscale"); w.Int(c.upscale_filter); w.Key("downscale"); w.Int(c.downscale_filter); w.Key("scaling"); w.Int(c.fullscreen_scaling);
	w.Key("introSound"); w.Bool(c.play_boot_sound);
	w.Key("audioApi"); w.Int(c.audio_api); w.Key("latency"); w.Int(c.audio_delay);
	w.Key("tvDevice"); w.Int(Impl::Token(s.output, c.tv_device)); w.Key("tvChannels"); w.Int(c.tv_channels); w.Key("tvVolume"); w.Int(c.tv_volume);
	w.Key("padDevice"); w.Int(Impl::Token(s.output, c.pad_device)); w.Key("padVolume"); w.Int(c.pad_volume);
	w.Key("micDevice"); w.Int(Impl::Token(s.input, c.input_device)); w.Key("micVolume"); w.Int(c.input_volume);
	const auto& o = s.overlay.overlay; const auto& n = s.overlay.notification;
	w.Key("overlayPosition"); w.Int(static_cast<int>(o.position));
	w.Key("overlayColor"); w.Uint(o.text_color); w.Key("overlayScale"); w.Int(o.text_scale);
	w.Key("overlayFps"); w.Bool(o.fps); w.Key("overlayDrawCalls"); w.Bool(o.drawcalls);
	w.Key("overlayCpu"); w.Bool(o.cpu_usage); w.Key("overlayCpuPerCore"); w.Bool(o.cpu_per_core_usage);
	w.Key("overlayRam"); w.Bool(o.ram_usage); w.Key("overlayVram"); w.Bool(o.vram_usage); w.Key("overlayDebug"); w.Bool(o.debug);
	w.Key("notificationPosition"); w.Int(static_cast<int>(n.position));
	w.Key("notificationColor"); w.Uint(n.text_color); w.Key("notificationScale"); w.Int(n.text_scale);
	w.Key("notificationProfiles"); w.Bool(n.controller_profiles); w.Key("notificationBattery"); w.Bool(n.controller_battery);
	w.Key("notificationShaders"); w.Bool(n.shader_compiling); w.Key("notificationFriends"); w.Bool(n.friends);
	w.EndObject();
	const auto list = [&](const char* key, const auto& devices) {
		w.Key(key); w.StartArray();
		for (const auto& device : devices) { w.StartObject(); w.Key("token"); w.Uint64(device.token); w.Key("name"); w.String(device.name.c_str()); w.EndObject(); }
		w.EndArray();
	};
	list("gpus", s.gpu); list("outputs", s.output); list("inputs", s.input);
	w.Key("audioApis"); w.StartArray(); for (auto api : s.apis) w.Int(api); w.EndArray();
	w.Key("tvChannels"); w.StartArray(); for (auto channel : s.channels) w.Int(channel); w.EndArray();
	{
		std::shared_lock lock(g_audioMutex);
		w.Key("activeTvVolume"); if (g_tvAudio) w.Int(g_tvAudio->GetVolume()); else w.Null();
		w.Key("activeTvChannels"); if (g_tvAudio) w.Int(g_tvAudio->GetChannels()); else w.Null();
		w.Key("activePadVolume"); if (g_padAudio) w.Int(g_padAudio->GetVolume()); else w.Null();
		w.Key("tvStream"); w.Bool(bool(g_tvAudio)); w.Key("padStream"); w.Bool(bool(g_padAudio));
		w.Key("activeAudioApi"); if (g_tvAudio) w.Int(g_tvAudio->GetType()); else w.Null();
	}
	{
		std::shared_lock lock(g_audioInputMutex);
		w.Key("micStream"); w.Bool(bool(g_inputAudio));
	}
	w.EndObject(); return buffer.GetString();
}

std::string AvSettings::Command(std::string_view payload, bool running)
{
	auto& s = *m_impl;
	try
	{
		RequireAv(IsHealthy() && payload.size() <= 512 && payload.find('\0') == std::string_view::npos);
		rapidjson::Document request; request.Parse(payload.data(), payload.size());
		RequireAv(!request.HasParseError() && request.IsObject() && request.MemberCount() == 3 && request.HasMember("key") && request["key"].IsString() && request.HasMember("value") && request.HasMember("revision") && request["revision"].IsUint64() && request["revision"].GetUint64() == s.revision);
		std::set<std::string> keys;
		for (auto it = request.MemberBegin(); it != request.MemberEnd(); ++it)
		{
			const std::string member(it->name.GetString(), it->name.GetStringLength());
			RequireAv((member == "key" || member == "value" || member == "revision") && keys.insert(member).second);
		}
		const std::string key(request["key"].GetString(), request["key"].GetStringLength());
		RequireAv(key != "introSound");
		const auto& value = request["value"];
		const bool graphics = key == "graphicsApi" || key == "gpu" || key == "vsync" || key == "async" || key == "fullSync" || key == "upscale" || key == "downscale" || key == "scaling";
		RequireAv(!running || !graphics);
		const auto integer = [&](int lo, int hi) { RequireAv(value.IsInt() && value.GetInt() >= lo && value.GetInt() <= hi); return value.GetInt(); };
		const auto boolean = [&] { RequireAv(value.IsBool()); return value.GetBool() ? "true" : "false"; };
		s.Refresh(true);
		const auto original = ReadAv(s.path);
		pugi::xml_document document; RequireAv(document.load_buffer(original.data(), original.size()));
		auto root = document.child("content"); RequireAv(bool(root));
		auto graphic = root.child("Graphic"), audio = root.child("Audio"); RequireAv(graphic && audio);
		const auto& c = GetConfig();
		s.notice = "ready";
		SetAv(root, "play_boot_sound", "true");
		if (key.starts_with("overlay") || key.starts_with("notification"))
		{
			const bool performance = key.starts_with("overlay");
			auto node = graphic.child(performance ? "Overlay" : "Notification");
			if (!node) node = graphic.append_child(performance ? "Overlay" : "Notification");
			const auto suffix = key.substr(performance ? 7 : 12);
			if (suffix == "Position") SetAv(node, "Position", std::to_string(integer(0, 6)));
			else if (suffix == "Scale") { const auto scale = integer(50, 300); RequireAv(scale % 25 == 0); SetAv(node, "TextScale", std::to_string(scale)); }
			else if (suffix == "Color") { RequireAv(value.IsUint()); SetAv(node, "TextColor", std::to_string(value.GetUint())); }
			else
			{
				const std::map<std::string, const char*> fields = performance ? std::map<std::string, const char*>{{"Fps","FPS"},{"DrawCalls","DrawCalls"},{"Cpu","CPUUsage"},{"CpuPerCore","CPUPerCoreUsage"},{"Ram","RAMUsage"},{"Vram","VRAMUsage"},{"Debug","Debug"}} : std::map<std::string, const char*>{{"Profiles","ControllerProfiles"},{"Battery","ControllerBattery"},{"Shaders","ShaderCompiling"},{"Friends","FriendService"}};
				const auto field = fields.find(suffix); RequireAv(field != fields.end()); SetAv(node, field->second, boolean());
			}
		}
		else if (key == "graphicsApi")
		{
			const auto api = integer(0, 1); SetAv(graphic, "api", std::to_string(api));
			if (api == kOpenGL && c.vsync > 1) SetAv(graphic, "VSync", "1");
		}
		else if (key == "gpu")
		{
			RequireAv(c.graphic_api == kVulkan); const auto token = integer(0, INT_MAX);
			const auto it = std::find_if(s.gpu.begin(), s.gpu.end(), [&](const auto& d) { return d.token == token; });
			RequireAv(token == 0 || it != s.gpu.end());
			SetAv(graphic, "vkDevice", token == 0 ? std::string(32, '0') : boost::nowide::narrow(it->identifier));
		}
		else if (key == "vsync") SetAv(graphic, "VSync", std::to_string(integer(0, c.graphic_api == kOpenGL ? 1 : 3)));
		else if (key == "async") { RequireAv(c.graphic_api == kVulkan); SetAv(graphic, "AsyncCompile", boolean()); }
		else if (key == "fullSync") SetAv(graphic, "GX2DrawdoneSync", boolean());
		else if (key == "upscale" || key == "downscale") SetAv(graphic, key == "upscale" ? "UpscaleFilter" : "DownscaleFilter", std::to_string(integer(0, 3)));
		else if (key == "scaling") SetAv(graphic, "FullscreenScaling", std::to_string(integer(0, 1)));
		else if (key == "audioApi")
		{
			const auto api = integer(0, IAudioAPI::AudioAPIEnd - 1);
			RequireAv(std::find(s.apis.begin(), s.apis.end(), api) != s.apis.end());
			SetAv(audio, "api", std::to_string(api));
			// API ごとの既定 Identifier を列挙結果から選ぶ。無効な出力は維持する。
			const auto devices = IAudioAPI::GetDevices(static_cast<IAudioAPI::AudioAPI>(api));
			for (const auto& [name, selected] : {std::pair{"TVDevice", c.tv_device}, std::pair{"PadDevice", c.pad_device}})
			{
				if (selected.empty()) continue;
				const auto exact = std::find_if(devices.begin(), devices.end(), [&](const auto& d) { return d->GetIdentifier() == selected; });
				RequireAv(exact != devices.end() || !devices.empty());
				SetAv(audio, name, boost::nowide::narrow((exact == devices.end() ? devices.front() : *exact)->GetIdentifier()));
			}
		}
		else if (key == "latency") SetAv(audio, "delay", std::to_string(integer(0, IAudioAPI::kBlockCount - 1)));
		else if (key == "tvChannels")
		{
			const auto channel = integer(kMono, kSurround); s.RefreshChannels();
			RequireAv(std::find(s.channels.begin(), s.channels.end(), channel) != s.channels.end()); SetAv(audio, "TVChannels", std::to_string(channel));
		}
		else if (key == "tvVolume" || key == "padVolume" || key == "micVolume")
			SetAv(audio, key == "tvVolume" ? "TVVolume" : key == "padVolume" ? "PadVolume" : "InputVolume", std::to_string(integer(0, 100)));
		else if (key == "tvDevice" || key == "padDevice" || key == "micDevice")
		{
			const auto token = integer(0, INT_MAX); const auto& devices = key == "micDevice" ? s.input : s.output;
			const auto it = std::find_if(devices.begin(), devices.end(), [&](const auto& d) { return d.token == token; });
			RequireAv(token == 0 || it != devices.end());
			SetAv(audio, key == "tvDevice" ? "TVDevice" : key == "padDevice" ? "PadDevice" : "InputDevice", token == 0 ? "" : boost::nowide::narrow(it->identifier));
		}
		else RequireAv(false);
		if (key == "audioApi" || key == "tvDevice" || key == "padDevice")
		{
			const auto api = static_cast<IAudioAPI::AudioAPI>(audio.child("api").text().as_int());
			for (const auto* name : {"TVDevice", "PadDevice"})
			{
				if (key != "audioApi" && ((key == "tvDevice") != (std::string_view(name) == "TVDevice"))) continue;
				const auto identifier = boost::nowide::widen(audio.child_value(name));
				if (identifier.empty()) continue;
				const auto descriptions = IAudioAPI::GetDevices(api);
				const auto device = std::find_if(descriptions.begin(), descriptions.end(), [&](const auto& d) { return d->GetIdentifier() == identifier; });
				RequireAv(device != descriptions.end());
				const auto channel = std::string_view(name) == "TVDevice" ? c.tv_channels : kStereo;
				const auto probe = IAudioAPI::CreateDevice(api, *device, 48000, CemuConfig::AudioChannelsToNChannels(channel), 576, 16);
				RequireAv(bool(probe));
			}
		}
		// 候補だけを reload し、Core が読む unrelated 設定には触れない。
		const auto candidatePath = fs::path(s.path.wstring() + L".validate-" + std::to_wstring(GetCurrentProcessId()));
		std::ostringstream serialized; document.save(serialized);
		WriteAv(candidatePath, serialized.str());
		CemuConfig candidate;
		XMLConfig<CemuConfig> validation(candidate, candidatePath.wstring());
		const bool valid = validation.Load(); DeleteFileW(candidatePath.c_str()); RequireAv(valid);
		s.applyNotice = "applying";
		// AX の Feed/Play と MIC の Capture に対する既存の所有 mutex を利用する。
		std::unique_lock audioLock(g_audioMutex);
		std::unique_lock micLock(g_audioInputMutex);
		AudioAPIPtr tv, pad; AudioInputAPIPtr mic;
		const bool replaceTv = running && (key == "audioApi" || key == "tvDevice" || key == "tvChannels");
		const bool replacePad = running && (key == "audioApi" || key == "padDevice");
		const bool replaceMic = running && key == "micDevice" && bool(g_inputAudio);
		const auto createOutput = [&](const std::wstring& identifier, AudioChannels channels, int volume) -> AudioAPIPtr {
			if (identifier.empty()) return {};
			const auto api = static_cast<IAudioAPI::AudioAPI>(candidate.audio_api);
			const auto descriptions = IAudioAPI::GetDevices(api);
			const auto found = std::find_if(descriptions.begin(), descriptions.end(), [&](const auto& d) { return d->GetIdentifier() == identifier; });
			RequireAv(found != descriptions.end());
			auto stream = IAudioAPI::CreateDevice(api, *found, 48000, CemuConfig::AudioChannelsToNChannels(channels), 576, 16);
			RequireAv(bool(stream)); stream->SetVolume(volume); return stream;
		};
		if (replaceTv) tv = createOutput(candidate.tv_device, candidate.tv_channels, candidate.tv_volume);
		if (replacePad) pad = createOutput(candidate.pad_device, kStereo, candidate.pad_volume);
		if (replaceMic && !candidate.input_device.empty())
		{
			const auto descriptions = IAudioInputAPI::GetDevices(IAudioInputAPI::Cubeb);
			const auto found = std::find_if(descriptions.begin(), descriptions.end(), [&](const auto& d) { return d->GetIdentifier() == candidate.input_device; });
			RequireAv(found != descriptions.end());
			mic = IAudioInputAPI::CreateDevice(IAudioInputAPI::Cubeb, *found, 32000, 1, 96, 16); RequireAv(bool(mic));
			mic->SetVolume(candidate.input_volume);
			// 既に Capture 中の場合だけ再開し、設定操作によって新しい録音は開始しない。
			if (g_inputAudio->IsPlaying()) RequireAv(mic->Play());
		}
		if (replaceTv && tv) RequireAv(tv->Play());
		if (replacePad && pad) RequireAv(pad->Play());
		s.applyNotice = "applied";
		// 元 Stream はまだ保持し、保存が失敗した場合にも元の出力を継続する。
		s.Persist(document);
		if (replaceTv) { if (g_tvAudio) g_tvAudio->Stop(); g_tvAudio = std::move(tv); }
		if (replacePad) { if (g_padAudio) g_padAudio->Stop(); g_padAudio = std::move(pad); }
		if (replaceMic) { g_inputAudio->Stop(); g_inputAudio = std::move(mic); }
		auto& applied = GetConfig();
		if (graphics)
		{
			applied.graphic_api = candidate.graphic_api; applied.vk_graphic_device_uuid = candidate.vk_graphic_device_uuid;
			applied.vsync = candidate.vsync; applied.async_compile = candidate.async_compile; applied.gx2drawdone_sync = candidate.gx2drawdone_sync;
			applied.upscale_filter = candidate.upscale_filter; applied.downscale_filter = candidate.downscale_filter; applied.fullscreen_scaling = candidate.fullscreen_scaling;
		}
		else if (key.starts_with("overlay") || key.starts_with("notification"))
		{
			s.overlay = {candidate.overlay, candidate.notification, s.revision}; LatteOverlay_setSettings(s.overlay);
		}
		else
		{
			applied.audio_api = candidate.audio_api; applied.audio_delay = candidate.audio_delay;
			applied.tv_channels = candidate.tv_channels; applied.tv_device = candidate.tv_device; applied.pad_device = candidate.pad_device; applied.input_device = candidate.input_device;
			applied.tv_volume = candidate.tv_volume; applied.pad_volume = candidate.pad_volume; applied.input_volume = candidate.input_volume;
			if (g_tvAudio) g_tvAudio->SetVolume(applied.tv_volume);
			if (g_padAudio) g_padAudio->SetVolume(applied.pad_volume);
			if (g_inputAudio) g_inputAudio->SetVolume(applied.input_volume);
			g_padVolume = applied.pad_volume; IAudioAPI::UpdateAudioDelay();
		}
		s.appliedRevision = s.revision; s.applyNotice = "applied-and-saved";
		micLock.unlock(); audioLock.unlock();
		s.Refresh(true); s.RefreshChannels();
	}
	catch (...) { s.applyNotice = s.notice == "recovery-required" ? "recovery-failed" : "previous-settings-retained"; if (s.notice != "save-failed" && s.notice != "recovery-required") s.notice = "rejected"; }
	return Snapshot();
}

void AvSettings::EndTitle()
{
	// PPC/GPU 停止後に入力 Stream を閉じ、次回 MICInit で保存済みデバイスを使う。
	std::unique_lock lock(g_audioInputMutex);
	if (g_inputAudio) { g_inputAudio->Stop(); g_inputAudio.reset(); }
}

void AvSettings::PrepareLaunch()
{
	RequireAv(IsHealthy());
	GetConfig().play_boot_sound = true;
	m_impl->Refresh(true);
	IAudioAPI::UpdateAudioDelay();
}
