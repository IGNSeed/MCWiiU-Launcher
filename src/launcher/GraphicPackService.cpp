#include "GraphicPackService.h"

#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/CafeSystem.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "util/IniParser/IniParser.h"
#include <Windows.h>
#include <objbase.h>
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/HW/Latte/Core/MinecraftNativeResolution.h"
#include <pugixml.hpp>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <openssl/sha.h>
#include <boost/algorithm/string.hpp>

namespace
{
	using Writer = rapidjson::Writer<rapidjson::StringBuffer>;
	void Require(bool value, const std::string& reason) { if (!value) throw std::runtime_error(reason); }
	std::string Utf8(const fs::path& path) { return _pathToUtf8(path); }
	std::string Key(std::string value)
	{
		std::replace(value.begin(), value.end(), '\\', '/');
		for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
		return value;
	}
	std::string Hash(std::string_view text)
	{
		std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
		SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), digest.data());
		std::string result;
		for (auto byte : digest) result += fmt::format("{:02x}", byte);
		return result;
	}
	void Text(Writer& writer, const char* key, std::string_view value)
	{
		writer.Key(key); writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
	}
	std::string String(const rapidjson::Value& value)
	{
		Require(value.IsString() && value.GetStringLength() <= 4096, "invalid-request");
		std::string result(value.GetString(), value.GetStringLength());
		Require(result.find('\0') == std::string::npos, "invalid-request");
		return result;
	}
	void Keys(const rapidjson::Value& json, std::initializer_list<const char*> keys)
	{
		Require(json.IsObject() && json.MemberCount() == keys.size(), "invalid-request");
		std::set<std::string> seen;
		for (auto it = json.MemberBegin(); it != json.MemberEnd(); ++it) Require(seen.insert(String(it->name)).second, "invalid-request");
		for (auto key : keys) Require(json.HasMember(key), "invalid-request");
	}
	void CheckPath(fs::path path)
	{
		Require(path.is_absolute() && path.lexically_normal() == path, "unsafe-path");
		for (;; path = path.parent_path())
		{
			const auto attributes = GetFileAttributesW(path.c_str());
			if (attributes == INVALID_FILE_ATTRIBUTES) Require(GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND, "unsafe-path");
			else Require(!(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "unsafe-path");
			if (path == path.root_path()) break;
		}
	}
	struct Handles
	{
		std::vector<HANDLE> files;
		~Handles() { for (auto file : files) CloseHandle(file); }
		void Pin(const fs::path& path, bool directory)
		{
			CheckPath(path);
			const auto handle = CreateFileW(path.c_str(), directory ? FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES : GENERIC_READ,
				directory ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ, nullptr, OPEN_EXISTING,
				FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr);
			Require(handle != INVALID_HANDLE_VALUE, "file-busy-or-unsafe"); files.push_back(handle);
			BY_HANDLE_FILE_INFORMATION info{};
			Require(GetFileInformationByHandle(handle, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && (directory || info.nNumberOfLinks == 1), "unsafe-file");
		}
		void Parents(fs::path path)
		{
			for (;; path = path.parent_path())
			{
				Pin(path, true);
				if (path == path.root_path()) break;
			}
		}
	};
	std::string Read(const fs::path& path, size_t limit = 4 * 1024 * 1024)
	{
		Handles pin; pin.Parents(path.parent_path()); pin.Pin(path, false);
		LARGE_INTEGER size{}; Require(GetFileSizeEx(pin.files.back(), &size) && size.QuadPart >= 0 && uint64(size.QuadPart) <= limit, "file-size-limit");
		std::string text(static_cast<size_t>(size.QuadPart), '\0');
		DWORD count = 0; Require(ReadFile(pin.files.back(), text.data(), static_cast<DWORD>(text.size()), &count, nullptr) && count == text.size(), "read-failed");
		return text;
	}
	void Write(const fs::path& path, std::string_view bytes)
	{
		CheckPath(path); Handles pin;
		const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		Require(file != INVALID_HANDLE_VALUE, "save-failed"); pin.files.push_back(file);
		DWORD count = 0;
		Require(WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) && count == bytes.size() && FlushFileBuffers(file), "save-failed");
	}
	bool SamePath(std::string_view left, const fs::path& right)
	{
		return Key(Utf8(_utf8ToPath(left).lexically_normal())) == Key(Utf8(right.lexically_normal()));
	}
} // namespace

struct GraphicPackService::Impl
{
	struct Pack
	{
		std::string id, digest;
		std::shared_ptr<GraphicPack2> core;
	};
	fs::path root = ActiveSettings::GetUserDataPath("graphicPacks"), settings = GetConfigHandle().GetFilename();
	std::vector<Pack> packs;
	std::unique_ptr<Handles> sessionPins;
	std::map<std::string, std::string> replacements;
	std::string epoch, notice, problem;
	uint64_t revision = 1, saveSequence = 0;
	bool healthy = true, running = false;

	std::string Revision() const { return epoch + "-" + std::to_string(revision); }
	void ValidateSettings(pugi::xml_document& document, std::string& original) const
	{
		original = Read(settings);
		Require(document.load_buffer(original.data(), original.size(), pugi::parse_full) && document.child("content") && !document.child("content").next_sibling("content"), "settings-corrupt");
		Require(!document.child("content").child("GraphicPack").next_sibling("GraphicPack"), "settings-corrupt");
	}
	void Save(const Pack& pack)
	{
		auto configLock = GetConfigHandle().Lock();
		Handles directories; directories.Parents(settings.parent_path());
		pugi::xml_document document; std::string original;
		ValidateSettings(document, original);
		auto graphic = document.child("content").child("GraphicPack");
		if (!graphic) graphic = document.child("content").append_child("GraphicPack");
		const auto relative = _utf8ToPath(pack.core->GetNormalizedPathString());
		// 旧絶対パス・旧形式の同一パック記録だけをまとめ、他の設定を保持する。
		for (auto entry = graphic.child("Entry"); entry;)
		{
			auto next = entry.next_sibling("Entry");
			const auto filename = entry.attribute("filename") ? entry.attribute("filename").value() : entry.child_value("filename");
			if (SamePath(filename, relative) || SamePath(filename, pack.core->GetRulesPath())) graphic.remove_child(entry);
			entry = next;
		}
		auto entry = graphic.append_child("Entry");
		entry.append_attribute("filename") = Utf8(relative).c_str();
		if (!pack.core->IsEnabled()) entry.append_attribute("disabled") = true;
		std::unordered_map<std::string, std::string> selection;
		for (const auto& preset : pack.core->GetActivePresets())
		{
			Require(!boost::iequals(preset->category, "_disabled"), "reserved-category");
			selection[preset->category] = preset->name;
			auto node = entry.append_child("Preset");
			node.append_child("category").text() = preset->category.c_str();
			node.append_child("preset").text() = preset->name.c_str();
		}
		if (!pack.core->IsEnabled()) selection["_disabled"] = "true";
		std::ostringstream stream; document.save(stream); const auto bytes = stream.str();
		const auto suffix = _utf8ToPath(Revision() + "-" + std::to_string(++saveSequence)).wstring();
		const auto temporary = fs::path(settings.wstring() + L".graphic-packs-" + suffix);
		const auto backup = fs::path(settings.wstring() + L".graphic-packs-backup-" + suffix);
		auto candidateEntries = GetConfig().graphic_pack_entries;
		for (auto it = candidateEntries.begin(); it != candidateEntries.end();)
			if (SamePath(Utf8(it->first), relative) || SamePath(Utf8(it->first), pack.core->GetRulesPath())) it = candidateEntries.erase(it); else ++it;
		candidateEntries[relative] = std::move(selection);
		try
		{
			Write(backup, original); Write(temporary, bytes);
			pugi::xml_document verified; const auto written = Read(temporary);
			Require(written == bytes && verified.load_buffer(written.data(), written.size()), "save-failed");
			Require(Read(settings) == original, "settings-changed");
			Require(MoveFileExW(temporary.c_str(), settings.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "save-failed");
		}
		catch (...) { DeleteFileW(temporary.c_str()); throw; }
		GetConfig().graphic_pack_entries.swap(candidateEntries);
		++revision;
	}
	void Scan(const fs::path& directory, size_t depth, size_t& count)
	{
		Require(depth <= 24 && ++count <= 16384, "folder-limit");
		CheckPath(directory);
		if (fs::is_regular_file(directory / "rules.txt"))
		{
			try
			{
				const auto rules = Read(directory / "rules.txt", 1024 * 1024);
				Require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, rules.data(), static_cast<int>(rules.size()), nullptr, 0) > 0, "invalid-rules-encoding");
				const auto before = GraphicPack2::GetGraphicPacks().size();
				GraphicPack2::LoadGraphicPack(directory);
				if (before == GraphicPack2::GetGraphicPacks().size()) { notice = "invalid-pack-skipped"; return; }
				auto core = GraphicPack2::GetGraphicPacks().back();
				if (!core->MatchesTitle(0x00050000101dbe00ULL)) { core->SetEnabled(false); return; }
				Require(core->GetPresets().size() <= 512, "preset-limit");
				Require(core->GetName().size() <= 1024 && core->GetDescription().size() <= 16384, "metadata-limit");
				for (const auto& preset : core->GetPresets())
					Require(preset->category.size() <= 1024 && preset->name.size() <= 1024 && !boost::iequals(preset->category, "_disabled"), "invalid-preset-category");
				const auto& entries = GetConfig().graphic_pack_entries;
				bool known = false;
				for (const auto& [path, selection] : entries)
					if (SamePath(Utf8(path), core->GetRulesPath()) || SamePath(Utf8(path), _utf8ToPath(core->GetNormalizedPathString())))
					{
						known = true; bool enabled = true;
						for (const auto& [category, name] : selection)
							if (boost::iequals(category, "_disabled")) enabled = false; else core->SetActivePreset(category, name, false);
						core->SetEnabled(enabled); core->UpdatePresetVisibility(); core->ValidatePresetSelections(); break;
					}
				if (!known) core->SetEnabled(false);
				packs.push_back({Hash(Key(core->GetNormalizedPathString())), Hash(rules), core});
				Require(packs.size() <= 512, "pack-limit");
			}
			catch (const std::exception&)
			{
				for (const auto& core : GraphicPack2::GetGraphicPacks()) if (core->GetRulesPath() == directory / "rules.txt") core->SetEnabled(false);
				notice = "invalid-pack-skipped";
			}
			return;
		}
		for (const auto& entry : fs::directory_iterator(directory))
		{
			CheckPath(entry.path());
			if (entry.is_directory()) Scan(entry.path(), depth + 1, count);
		}
	}
	std::string Snapshot(bool canEdit, std::string_view result) const
	{
		rapidjson::StringBuffer buffer; Writer writer(buffer); writer.StartObject();
		Text(writer, "revision", Revision()); Text(writer, "notice", result); Text(writer, "problem", problem);
		writer.Key("canEdit"); writer.Bool(canEdit && healthy && !running);
		writer.Key("running"); writer.Bool(running);
		writer.Key("packs"); writer.StartArray();
		for (const auto& pack : packs)
		{
			writer.StartObject(); Text(writer, "id", pack.id); Text(writer, "name", pack.core->GetName()); Text(writer, "description", pack.core->GetDescription());
			writer.Key("enabled"); writer.Bool(pack.core->IsEnabled());
			writer.Key("categories"); writer.StartArray();
			std::vector<std::string> order; auto categories = pack.core->GetCategorizedPresets(order);
			for (const auto& category : order)
			{
				writer.StartObject(); Text(writer, "name", category); Text(writer, "selected", pack.core->GetActivePreset(category));
				writer.Key("options"); writer.StartArray();
				for (const auto& preset : categories[category]) if (preset->visible) writer.String(preset->name.c_str());
				writer.EndArray(); writer.EndObject();
			}
			writer.EndArray(); writer.EndObject();
		}
		writer.EndArray(); writer.EndObject(); return {buffer.GetString(), buffer.GetSize()};
	}
};

GraphicPackService::GraphicPackService() : m_impl(std::make_unique<Impl>())
{
	auto& state = *m_impl;
	GUID guid{}; CoCreateGuid(&guid); state.epoch = Hash({reinterpret_cast<const char*>(&guid), sizeof(guid)}).substr(0, 24);
	GraphicPack2::SetLauncherManaged(true);
	try
	{
		pugi::xml_document document; std::string original; state.ValidateSettings(document, original);
		CheckPath(state.root); Handles directories; directories.Parents(state.root.parent_path());
		fs::create_directories(state.root); directories.Pin(state.root, true);
		size_t count = 0; state.Scan(state.root, 0, count);
		std::sort(state.packs.begin(), state.packs.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
	}
	catch (const std::exception& error) { state.healthy = false; state.problem = error.what(); for (auto& pack : GraphicPack2::GetGraphicPacks()) pack->SetEnabled(false); }
}
GraphicPackService::~GraphicPackService() = default;
bool GraphicPackService::IsHealthy() const { return m_impl->healthy; }

std::string GraphicPackService::Command(std::string_view payload, bool canEdit)
{
	auto& state = *m_impl; std::string result = state.notice;
	try
	{
		Require(!payload.empty() && payload.size() <= 16384 && payload.find('\0') == std::string::npos, "invalid-request");
		rapidjson::Document json; json.Parse<rapidjson::kParseValidateEncodingFlag>(payload.data(), payload.size());
		Require(!json.HasParseError() && json.IsObject() && json.HasMember("op"), "invalid-request");
		const auto op = String(json["op"]);
		if (op == "list") Keys(json, {"op"});
		else
		{
			Require(canEdit && !state.running, "game-running-or-busy"); Require(state.healthy, "not-ready");
			if (op == "enable") Keys(json, {"op", "revision", "id", "enabled"});
			else if (op == "preset") Keys(json, {"op", "revision", "id", "category", "name"});
			else throw std::runtime_error("invalid-request");
			Require(String(json["revision"]) == state.Revision(), "stale-revision");
			const auto id = String(json["id"]);
			auto found = std::find_if(state.packs.begin(), state.packs.end(), [&](auto& pack) { return pack.id == id; });
			Require(found != state.packs.end(), "unknown-pack"); auto& pack = *found;
			const bool enabled = pack.core->IsEnabled(); std::vector<std::pair<bool, bool>> previous;
			for (const auto& preset : pack.core->GetPresets()) previous.emplace_back(preset->active, preset->visible);
			try
			{
				Require(Hash(Read(pack.core->GetRulesPath(), 1024 * 1024)) == pack.digest, "pack-changed-restart-required");
				if (op == "enable") { Require(json["enabled"].IsBool(), "invalid-request"); pack.core->SetEnabled(json["enabled"].GetBool()); }
				else
				{
					const auto category = String(json["category"]), name = String(json["name"]);
					const auto& presets = pack.core->GetPresets();
					Require(std::any_of(presets.begin(), presets.end(), [&](auto& p) { return p->category == category && p->name == name && p->visible; }), "invalid-preset");
					Require(pack.core->SetActivePreset(category, name), "invalid-preset");
					pack.core->UpdatePresetVisibility(); pack.core->ValidatePresetSelections();
				}
				state.Save(pack); result = "saved";
			}
			catch (...)
			{
				pack.core->SetEnabled(enabled);
				for (size_t i = 0; i < previous.size(); ++i) { pack.core->GetPresets()[i]->active = previous[i].first; pack.core->GetPresets()[i]->visible = previous[i].second; }
				throw;
			}
		}
	}
	catch (const std::exception& error) { result = error.what(); }
	return state.Snapshot(canEdit, result);
}

std::string GraphicPackService::OpenFolder(const std::function<bool(const fs::path&)>& open)
{
	auto& state = *m_impl; std::string result;
	try { Require(state.healthy, "not-ready"); Handles directories; directories.Parents(state.root); result = open(state.root) ? "folder-opened" : "folder-open-failed"; }
	catch (const std::exception& error) { result = error.what(); }
	return state.Snapshot(!state.running, result);
}

void GraphicPackService::PrepareLaunch(int frequency, unsigned width, unsigned height)
{
	auto& state = *m_impl; Require(state.healthy && !state.running, "Graphic Packs are unavailable");
	Require(GraphicPack2::GetActiveGraphicPacks().empty() && !GraphicPack2::TrackedModuleCount(), "Graphic pack session was not released");
	GraphicPack2::SetLauncherVSyncFrequency(frequency);
	state.running = true; state.replacements.clear(); state.sessionPins = std::make_unique<Handles>();
	state.sessionPins->Parents(state.root);
	pugi::xml_document document; std::string original; state.ValidateSettings(document, original);
	for (const auto& pack : state.packs)
	{
		if (!pack.core->IsEnabled()) continue;
		const auto folder = pack.core->GetRulesPath().parent_path(); state.sessionPins->Pin(folder, true);
		size_t count = 0; uint64_t bytes = 0; std::vector<fs::path> files;
		for (const auto& entry : fs::recursive_directory_iterator(folder))
		{
			Require(++count <= 16384 && entry.path().lexically_relative(folder).native().size() <= 2000, "Graphic pack folder limit exceeded");
			state.sessionPins->Pin(entry.path(), entry.is_directory());
			if (entry.is_regular_file()) { files.push_back(entry.path()); bytes += entry.file_size(); Require(bytes <= 1024ULL * 1024 * 1024, "Graphic pack size limit exceeded"); }
		}
		pack.core->SetLauncherFiles(std::move(files));
		auto rules = Read(pack.core->GetRulesPath(), 1024 * 1024);
		Require(Hash(rules) == pack.digest, "Graphic pack changed; restart Launcher: " + pack.core->GetName());
		IniParser parser(std::span<char>(rules.data(), rules.size()), Utf8(pack.core->GetRulesPath())); ExpressionParser expression; pack.core->AddConstantsForCurrentPreset(expression);
		while (parser.NextSection())
		{
			if (boost::iequals(parser.GetCurrentSectionName(), "Control"))
				if (auto value = parser.FindOption("vsyncFrequency"))
				{
					const auto vsync = expression.Evaluate<int>(*value);
					Require(vsync < 1 || vsync == (frequency ? frequency : 60), "Graphic pack VSync conflicts with Native Graphics: " + pack.core->GetName());
					// Original の 60Hz も custom として残さず、開始前に同値を扱う。
				}
			if (boost::iequals(parser.GetCurrentSectionName(), "TextureRedefine") && width && height)
			{
				const auto w = parser.FindOption("overwriteWidth"), h = parser.FindOption("overwriteHeight");
				if (w || h)
				{
					const auto filterW = parser.FindOption("width"), filterH = parser.FindOption("height");
					const int originalW = filterW ? expression.Evaluate<int>(*filterW) : -1, originalH = filterH ? expression.Evaluate<int>(*filterH) : -1;
					const bool touchesNative = originalW < 0 || originalH < 0 || (originalW == 1280 && originalH == 720) || ((originalW == 864 || originalW == 854) && originalH == 480);
					if (touchesNative)
					{
						Require(originalW > 0 && originalH > 0 && w && h && expression.Evaluate<int>(*w) == int(double(width) / 1280 * originalW) && expression.Evaluate<int>(*h) == int(double(height) / 720 * originalH),
							"Graphic pack texture dimensions conflict with Native Resolution: " + pack.core->GetName());
					}
				}
			}
		}
		for (const auto& entry : fs::recursive_directory_iterator(folder)) if (entry.is_regular_file())
		{
			const auto relative = entry.path().lexically_relative(folder); auto it = relative.begin(); if (it == relative.end()) continue;
			const auto part = Key(Utf8(*it++)); if (part != "content" && part != "code" && part != "aoc") continue;
			std::string virtualPath = part == "content" ? "/vol/content" : part == "code" ? CafeSystem::GetInternalVirtualCodeFolder() : "/vol/aoc0005000c101dbe00";
			while (virtualPath.ends_with('/')) virtualPath.pop_back();
			for (; it != relative.end(); ++it) virtualPath += "/" + Utf8(*it);
			virtualPath = Key(virtualPath);
			const auto [existing, inserted] = state.replacements.emplace(virtualPath, pack.core->GetName());
			Require(inserted, "Graphic pack replacement conflict: " + pack.core->GetName() + " / " + existing->second + " at " + virtualPath);
		}
	}
}

void GraphicPackService::ValidateReplacements(const std::vector<std::string>& texturePaths) const
{
	for (const auto& path : texturePaths)
		if (auto found = m_impl->replacements.find(Key(path)); found != m_impl->replacements.end())
			throw std::runtime_error("Texture / Graphic Pack conflict: " + found->second + " at " + found->first);
}

void GraphicPackService::EndTitle()
{
	auto& state = *m_impl;
	Require(GraphicPack2::GetActiveGraphicPacks().empty() && !GraphicPack2::TrackedModuleCount(), "Graphic pack cleanup failed");
	for (const auto& pack : state.packs) Require(!pack.core->IsActivated() && pack.core->GetPatchGroups().empty() && pack.core->GetCallbacks().empty() && !fscDeviceRedirect_ownedCount(pack.core.get()), "Graphic pack state remains after shutdown");
	Require(RPLLoader_GetCodeCaveAllocationCount() == 0, "Graphic pack Code Cave remains after shutdown");
	cemuLog_log(LogType::Force, "Graphic pack session released: active={} modules={} caves={}", GraphicPack2::GetActiveGraphicPacks().size(), GraphicPack2::TrackedModuleCount(), RPLLoader_GetCodeCaveAllocationCount());
	state.sessionPins.reset(); state.replacements.clear(); state.running = false;
}
