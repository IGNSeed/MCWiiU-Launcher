#include "InputService.h"

#include "config/ActiveSettings.h"
#include "input/InputManager.h"
#include "input/api/SDL/SDLController.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <Windows.h>
#include <cctype>
#include <fstream>

namespace
{
	using Writer = rapidjson::Writer<rapidjson::StringBuffer>;
	void RequireInput(bool valid) { if (!valid) throw std::runtime_error("input-operation-failed"); }
	bool ValidName(std::string_view name)
	{
		if (name.empty() || name.size() > 192 || name.front() == ' ' || name.back() == ' ') return false;
		if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0) <= 0) return false;
		for (const unsigned char c : name) if (c < 32 || std::string_view(".<>:\"/\\|?*").find(c) != std::string_view::npos) return false;
		std::string lower(name);
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (lower == "con" || lower == "prn" || lower == "aux" || lower == "nul" || lower == "default" || lower.starts_with("controller")) return false;
		if (lower.size() == 4 && (lower.starts_with("com") || lower.starts_with("lpt")) && lower[3] >= '0' && lower[3] <= '9') return false;
		return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0) <= 64;
	}
	void CheckPath(const fs::path& path)
	{
		for (auto current = path; !current.empty(); current = current.parent_path())
		{
			const auto attributes = GetFileAttributesW(current.c_str());
			RequireInput(attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT));
			if (current == current.parent_path()) break;
		}
	}
	std::string Read(const fs::path& path)
	{
		CheckPath(path);
		std::ifstream stream(path, std::ios::binary);
		RequireInput(stream.good() && fs::file_size(path) <= 65536);
		return {std::istreambuf_iterator<char>(stream), {}};
	}
	void WriteNew(const fs::path& path, const std::string& bytes)
	{
		CheckPath(path);
		const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		RequireInput(file != INVALID_HANDLE_VALUE);
		DWORD written = 0;
		const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(file);
		const bool closed = CloseHandle(file) != FALSE;
		if (!ok || !closed) { DeleteFileW(path.c_str()); RequireInput(false); }
	}
	std::string Xml(const pugi::xml_document& doc) { std::ostringstream stream; doc.save(stream); return stream.str(); }
	void Text(pugi::xml_node node, const char* key, const std::string& value)
	{
		auto child = node.child(key);
		if (!child) child = node.append_child(key);
		child.text().set(value.c_str());
	}
	void Validate(const pugi::xml_document& doc)
	{
		const auto root = doc.child("emulated_controller");
		RequireInput(root && std::string_view(root.child_value("type")) == "Wii U GamePad");
		const auto c = root.child("controller");
		RequireInput(c && !c.next_sibling("controller") && std::string_view(c.child_value("api")) == "SDLController");
		const std::string uuid = c.child_value("uuid");
		const auto separator = uuid.find('_');
		RequireInput(separator > 0 && separator < 8 && uuid.size() == separator + 33);
		RequireInput(std::all_of(uuid.begin(), uuid.begin() + separator, [](unsigned char ch) { return std::isdigit(ch); }));
		RequireInput(std::all_of(uuid.begin() + separator + 1, uuid.end(), [](unsigned char ch) { return std::isxdigit(ch); }));
		const std::string profile = root.child_value("profile");
		RequireInput(profile.empty() || profile == "default" || ValidName(profile));
		for (const auto group : {"axis", "rotation", "trigger"})
		{
			const auto setting = c.child(group);
			const float dz = setting.child("deadzone").text().as_float(-1), range = setting.child("range").text().as_float(-1);
			RequireInput(std::isfinite(dz) && dz >= 0 && dz <= 1 && std::isfinite(range) && range > 0 && range <= 10);
		}
		std::set<unsigned> ids;
		for (const auto entry : c.child("mappings").children("entry"))
		{
			const auto id = entry.child("mapping").text().as_uint(0), button = entry.child("button").text().as_uint(UINT_MAX);
			RequireInput(id > 0 && id < VPADController::kButtonId_Max && button < kButtonMAX && ids.insert(id).second);
		}
	}
	void Vector(Writer& writer, const glm::vec2& value)
	{
		writer.StartArray(); writer.Double(value.x); writer.Double(value.y); writer.EndArray();
	}
} // namespace

struct InputService::Impl
{
	struct Device { uint64_t token; std::shared_ptr<SDLController> controller; };
	std::vector<Device> devices;
	std::shared_ptr<SDLController> selected;
	std::string fallbackXml, selectedUuid, selectedSerial, notice = "ready";
	uint64_t revision = 1, tokenCounter = 0, fileCounter = 0, captureId = 0, captureSequence = 0;
	unsigned captureMapping = 0;
	bool neutral = false, continuous = false, advancing = false, initialized = false, editable = true, autoSelect = true;
	std::set<unsigned> completed;
	std::map<unsigned, unsigned> sequenceButtons;
	static constexpr unsigned kBasicMappings = 24;
	std::chrono::steady_clock::time_point deadline{}, nextDevices{};
	fs::path directory = ActiveSettings::GetConfigPath("controllerProfiles");

	pugi::xml_document Current()
	{
		pugi::xml_document doc;
		const auto pad = InputManager::instance().get_controller(0);
		if (!pad) { doc.append_child("emulated_controller"); Text(doc.document_element(), "type", "Wii U GamePad"); return doc; }
		const auto xml = InputManager::instance().serialize_profile(0, pad->get_profile_name());
		RequireInput(xml && doc.load_buffer(xml->data(), xml->size()));
		if (!doc.document_element().child("controller") && !fallbackXml.empty()) RequireInput(doc.load_buffer(fallbackXml.data(), fallbackXml.size()));
		return doc;
	}
	void Bind(const pugi::xml_document& doc)
	{
		const auto pad = InputManager::instance().get_vpad_controller(0);
		RequireInput(bool(pad));
		pad->clear_mappings(); pad->clear_controllers();
		if (!selected) return;
		const auto c = doc.child("emulated_controller").child("controller");
		auto settings = selected->get_settings();
		for (const auto& [name, target] : std::array<std::pair<const char*, ControllerBase::AxisSetting*>, 3>{{{"axis", &settings.axis}, {"rotation", &settings.rotation}, {"trigger", &settings.trigger}}})
		{
			target->deadzone = c.child(name).child("deadzone").text().as_float(0.25f);
			target->range = c.child(name).child("range").text().as_float(1);
		}
		settings.rumble = c.child("rumble").text().as_float();
		settings.motion = c.child("motion").text().as_bool();
		selected->set_settings(settings);
		pad->add_controller(selected);
		for (const auto entry : c.child("mappings").children("entry"))
			pad->set_mapping(entry.child("mapping").text().as_uint(), selected, entry.child("button").text().as_uint());
	}
	fs::path DeviceProfile(const pugi::xml_document& doc)
	{
		const auto c = doc.child("emulated_controller").child("controller");
		const std::string uuid = c.child_value("uuid");
		const auto separator = uuid.find('_');
		RequireInput(separator != std::string::npos && uuid.size() == separator + 33);
		uint64_t hash = 14695981039346656037ULL;
		for (const unsigned char ch : std::string(c.child_value("launcher_serial"))) { hash ^= ch; hash *= 1099511628211ULL; }
		return directory / ".launcher-devices" / fmt::format("{}-{:016x}.xml", uuid.substr(separator + 1), hash);
	}
	void Select(const std::shared_ptr<SDLController>& controller)
	{
		RequireInput(controller->connect());
		auto doc = Current();
		const auto previous = selected;
		const auto oldUuid = std::string(doc.child("emulated_controller").child("controller").child_value("uuid"));
		bool compatible = true;
		try { Validate(doc); } catch (...) { compatible = false; }
		const bool sameModel = compatible && oldUuid.find('_') != std::string::npos && oldUuid.substr(oldUuid.find('_')) == controller->uuid().substr(controller->uuid().find('_'));
		try
		{
			// 機種を変更する前に元の XML を機器別に残し、名前付き Profile は更新しない。
			if (compatible && !sameModel && std::string_view(doc.child("emulated_controller").child("controller").child_value("api")) == "SDLController")
			{
				try { Validate(doc); const auto oldPath = DeviceProfile(doc); CheckPath(oldPath); fs::create_directories(oldPath.parent_path());
					if (!fs::exists(oldPath)) WriteNew(oldPath, Xml(doc)); } catch (...) { RequireInput(false); }
			}
			selected = controller;
			if (!sameModel)
			{
				auto defaults = std::make_shared<VPADController>(0);
				defaults->add_controller(selected); RequireInput(defaults->set_default_mapping(selected));
				auto& manager = InputManager::instance(); const auto old = manager.get_controller(0);
				manager.set_controller(defaults);
				try { doc = Current(); } catch (...) { if (old) manager.set_controller(old); else manager.delete_controller(0); throw; }
				if (old) manager.set_controller(old); else manager.delete_controller(0);
				Text(doc.document_element().child("controller"), "launcher_serial", selected->serial());
				const auto savedPath = DeviceProfile(doc);
				if (fs::exists(savedPath))
				{
					const auto bytes = Read(savedPath); RequireInput(doc.load_buffer(bytes.data(), bytes.size())); Validate(doc);
					RequireInput(DeviceProfile(doc) == savedPath);
				}
				Text(doc.document_element(), "profile", "");
			}
			const auto c = doc.document_element().child("controller");
			Text(c, "uuid", selected->uuid()); Text(c, "display_name", selected->display_name()); Text(c, "launcher_serial", selected->serial());
			Persist(doc, sameModel);
			selectedUuid = selected->uuid(); selectedSerial = selected->serial();
			if (!sameModel) notice = "connected-new-model";
		}
		catch (...) { selected = previous; throw; }
	}
	void RefreshDevices(const std::function<void()>& beforeReconnect = {})
	{
		if (std::chrono::steady_clock::now() < nextDevices) return;
		nextDevices = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		auto provider = std::dynamic_pointer_cast<SDLControllerProvider>(InputManager::instance().get_api_provider(InputAPI::SDLController));
		if (!provider) return;
		std::vector<Device> fresh;
		for (const auto& item : provider->get_pinned_controllers())
		{
			auto c = std::static_pointer_cast<SDLController>(item);
			const auto id = c->instance_id();
			const auto old = std::find_if(devices.begin(), devices.end(), [&](const auto& d) { return d.controller->instance_id() == id; });
			fresh.push_back(old == devices.end() ? Device{++tokenCounter, c} : *old);
		}
		devices = std::move(fresh);
		AutoConnect(beforeReconnect);
	}
	void AutoConnect(const std::function<void()>& beforeReconnect = {})
	{
		if (!initialized || !autoSelect || notice == "recovery-required" || (selected && selected->is_connected())) return;
		captureId = 0; continuous = advancing = false;
		std::vector<std::shared_ptr<SDLController>> valid, serialMatches, modelMatches;
		for (const auto& d : devices)
		{
			if (!d.controller->supports_standard_gamepad()) continue;
			valid.push_back(d.controller);
			if (!selectedSerial.empty() && d.controller->serial() == selectedSerial && d.controller->uuid().substr(d.controller->uuid().find('_')) == selectedUuid.substr(selectedUuid.find('_'))) serialMatches.push_back(d.controller);
			const auto uuid = d.controller->uuid();
			if (selectedUuid.find('_') != std::string::npos && uuid.substr(uuid.find('_')) == selectedUuid.substr(selectedUuid.find('_'))) modelMatches.push_back(d.controller);
		}
		const auto candidate = serialMatches.size() == 1 ? serialMatches.front() : selectedSerial.empty() && modelMatches.size() == 1 ? modelMatches.front() : valid.size() == 1 ? valid.front() : nullptr;
		if (candidate && editable)
		{
			try { if (beforeReconnect) beforeReconnect(); Select(candidate); if (notice == "saved") notice = "reconnected"; }
			catch (...) { if (notice != "recovery-required") notice = "save-failed"; }
		}
		else notice = valid.empty() ? "disconnected" : "select-controller";
	}

	// 既存 save の serializer と load を利用し、ファイルの置換と復旧を Launcher が所有する。
	void Persist(pugi::xml_document& doc, bool updateNamed, bool newName = false)
	{
		if (selected) Text(doc.child("emulated_controller").child("controller"), "launcher_serial", selected->serial());
		Validate(doc);
		auto& input = InputManager::instance();
		RequireInput(!input.is_gameprofile_set(0));
		CheckPath(directory); fs::create_directories(directory);
		const auto suffix = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(++fileCounter);
		const auto stageName = ".launcher-stage-" + suffix;
		const auto stage = directory / (stageName + ".xml");
		const auto bytes = Xml(doc);
		const std::string profile = doc.child("emulated_controller").child_value("profile");
		std::vector<fs::path> targets{directory / "controller0.xml"};
		if (updateNamed && ValidName(profile)) targets.push_back(directory / _utf8ToPath(profile + ".xml"));
		if (newName) RequireInput(targets.size() == 2 && !fs::exists(targets.back()));
		const auto devicePath = DeviceProfile(doc);
		CheckPath(devicePath); fs::create_directories(devicePath.parent_path()); targets.push_back(devicePath);
		const auto old = input.get_controller(0);
		const auto oldSettings = selected ? selected->get_settings() : ControllerBase::Settings{};
		std::vector<std::pair<fs::path, std::optional<std::string>>> originals;
		std::vector<fs::path> replaced;
		try
		{
			const auto backups = directory / ".launcher-backups";
			CheckPath(backups); fs::create_directories(backups);
			for (const auto& target : targets)
			{
				CheckPath(target);
				originals.emplace_back(target, fs::exists(target) ? std::optional{Read(target)} : std::nullopt);
				if (originals.back().second)
					WriteNew(backups / (target.stem().wstring() + L"-" + _utf8ToPath(suffix).wstring() + L".xml"), *originals.back().second);
			}
			WriteNew(stage, bytes);
			RequireInput(Read(stage) == bytes && input.load(0, stageName));
			Bind(doc);
			// 再読込後にも全 mapping と左右の設定が一致することを確認する。
			const auto loaded = Current();
			const auto expected = doc.child("emulated_controller").child("controller"), actual = loaded.child("emulated_controller").child("controller");
			for (const auto entry : expected.child("mappings").children("entry"))
				RequireInput(InputManager::instance().get_vpad_controller(0)->get_mapping_name(entry.child("mapping").text().as_uint()) == selected->get_button_name(entry.child("button").text().as_uint()));
			RequireInput(expected.child("axis").child("deadzone").text().as_float() == actual.child("axis").child("deadzone").text().as_float() && expected.child("rotation").child("deadzone").text().as_float() == actual.child("rotation").child("deadzone").text().as_float());
			for (const auto& target : targets)
			{
				const auto temporary = fs::path(target.wstring() + L".pending-" + _utf8ToPath(suffix).wstring());
				WriteNew(temporary, bytes);
				if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(temporary.c_str()); RequireInput(false); }
				replaced.push_back(target);
				RequireInput(Read(target) == bytes);
			}
			RequireInput(input.load(0)); Bind(doc);
			DeleteFileW(stage.c_str()); fallbackXml = bytes; ++revision; notice = "saved";
		}
		catch (...)
		{
			if (old) input.set_controller(old); else input.delete_controller(0);
			if (selected) selected->set_settings(oldSettings);
			bool restored = true;
			for (const auto& target : replaced)
			{
				const auto it = std::find_if(originals.begin(), originals.end(), [&](const auto& p) { return p.first == target; });
				if (it->second)
				{
					const auto rollback = fs::path(target.wstring() + L".rollback-" + _utf8ToPath(suffix).wstring());
					try { WriteNew(rollback, *it->second); restored = bool(MoveFileExW(rollback.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) && restored; }
					catch (...) { restored = false; }
				}
				else restored = bool(DeleteFileW(target.c_str())) && restored;
			}
			DeleteFileW(stage.c_str()); notice = restored ? "save-failed" : "recovery-required";
			throw;
		}
	}

	void Capture(unsigned mapping, bool sequence = false)
	{
		RequireInput(selected && selected->is_connected() && mapping > 0 && mapping < VPADController::kButtonId_Max);
		captureMapping = mapping; captureId = ++captureSequence; neutral = false;
		continuous = sequence && mapping <= kBasicMappings; advancing = false; completed.clear(); sequenceButtons.clear();
		deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10); notice = "release-inputs";
	}
	ControllerState Raw()
	{
		if (!selected || !selected->is_connected()) { captureId = 0; continuous = advancing = false; return {}; }
		// SDLController::raw_state は既存 controller mutex 下で値をコピーする。
		return static_cast<ControllerBase&>(*selected).raw_state();
	}
	void CaptureStep(const ControllerState& raw)
	{
		if (!captureId) return;
		if (std::chrono::steady_clock::now() >= deadline) { captureId = 0; notice = "capture-timeout"; return; }
		if (!neutral)
		{
			neutral = raw.buttons.IsIdle() && glm::length(raw.axis) < 0.2f && glm::length(raw.rotation) < 0.2f && glm::length(raw.trigger) < 0.2f;
			if (neutral)
			{
				if (advancing) { ++captureMapping; advancing = false; }
				notice = "press-input";
			}
			return;
		}
		std::optional<unsigned> button;
		const auto buttons = raw.buttons.GetButtonList();
		if (!buttons.empty()) button = buttons.front();
		for (const auto& [axis, positive, negative] : std::array<std::tuple<glm::vec2, unsigned, unsigned>, 3>{{{raw.axis, kAxisXP, kAxisXN}, {raw.rotation, kRotationXP, kRotationXN}, {raw.trigger, kTriggerXP, kTriggerXN}}})
		{
			if (std::abs(axis.x) >= 0.6f) button = axis.x > 0 ? positive : negative;
			if (std::abs(axis.y) >= 0.6f) button = axis.y > 0 ? positive + 1 : negative + 1;
		}
		if (!button) return;
		if (continuous && std::any_of(sequenceButtons.begin(), sequenceButtons.end(), [&](const auto& value) { return value.first != captureMapping && value.second == *button; }))
		{
			neutral = false; notice = "duplicate-input"; return;
		}
		auto doc = Current(); auto mappings = doc.child("emulated_controller").child("controller").child("mappings");
		bool duplicate = false;
		for (auto entry = mappings.child("entry"); entry;)
		{
			const auto next = entry.next_sibling("entry");
			const auto id = entry.child("mapping").text().as_uint();
			if (id != captureMapping && entry.child("button").text().as_uint(UINT_MAX) == *button) duplicate = true;
			if (id == captureMapping) mappings.remove_child(entry);
			entry = next;
		}
		auto entry = mappings.append_child("entry"); Text(entry, "mapping", std::to_string(captureMapping)); Text(entry, "button", std::to_string(*button));
		Persist(doc, true);
		completed.insert(captureMapping); sequenceButtons[captureMapping] = *button;
		if (continuous && captureMapping < kBasicMappings)
		{
			advancing = true; neutral = false; notice = "release-inputs";
			deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		}
		else { captureId = 0; continuous = false; if (duplicate) notice = "saved-duplicate"; }
	}
};

InputService::InputService() : m_impl(std::make_unique<Impl>())
{
	auto& s = *m_impl; s.RefreshDevices();
	try
	{
		auto doc = s.Current(); Validate(doc);
		if (fs::exists(s.directory / "controller0.xml"))
		{
			pugi::xml_document stored;
			const auto bytes = Read(s.directory / "controller0.xml");
			if (stored.load_buffer(bytes.data(), bytes.size()))
			{
				const auto saved = stored.child("emulated_controller").child("controller");
				const auto current = doc.child("emulated_controller").child("controller");
				if (std::string_view(saved.child_value("uuid")) == current.child_value("uuid")) Text(current, "launcher_serial", saved.child_value("launcher_serial"));
			}
		}
		s.fallbackXml = Xml(doc);
		const auto c = doc.child("emulated_controller").child("controller");
		s.selectedUuid = c.child_value("uuid"); s.selectedSerial = c.child_value("launcher_serial");
		s.Bind(doc);
		s.notice = "select-controller";
	}
	catch (...)
	{
		s.notice = "unsupported-profile";
		const auto controller = InputManager::instance().get_controller(0);
		// 他 API や複数機器の既存 Profile は、明示選択するまで保持する。
		s.autoSelect = !controller || !controller->get_highest_mapping_id();
	}
	s.initialized = true; s.nextDevices = {}; s.RefreshDevices();
}
InputService::~InputService() = default;
void InputService::CancelCapture()
{
	if (m_impl->captureId) m_impl->notice = "capture-cancelled";
	m_impl->captureId = 0; m_impl->continuous = m_impl->advancing = false;
}
// 既存の native status 周期から呼び、Settings の表示状態に依存せず再接続する。
bool InputService::RefreshConnection(const std::function<void()>& beforeReconnect)
{
	auto& s = *m_impl;
	s.editable = true;
	if (s.autoSelect && (!s.selected || !s.selected->is_connected())) beforeReconnect();
	s.RefreshDevices(beforeReconnect);
	if (!s.selected || !s.selected->is_connected()) return false;
	const auto raw = s.Raw();
	return raw.buttons.IsIdle() && glm::length(raw.axis) < 0.2f && glm::length(raw.rotation) < 0.2f && glm::length(raw.trigger) < 0.2f;
}
bool InputService::IsHealthy() const { return m_impl->notice != "recovery-required"; }

std::string InputService::Snapshot(bool editable)
{
	auto& s = *m_impl; s.editable = editable; s.RefreshDevices();
	rapidjson::StringBuffer buffer; Writer w(buffer); w.StartObject();
	w.Key("notice"); w.String(s.notice.c_str());
	w.Key("revision"); w.Uint64(s.revision);
	w.Key("connected"); w.Bool(s.selected && s.selected->is_connected());
	w.Key("devices"); w.StartArray();
	for (const auto& d : s.devices)
	{
		w.StartObject(); w.Key("token"); w.Uint64(d.token); w.Key("name"); w.String(d.controller->display_name().c_str());

		w.Key("selected"); w.Bool(s.selected == d.controller && s.selected->is_connected()); w.EndObject();
	}
	w.EndArray(); w.Key("profiles"); w.StartArray();
	for (const auto& name : InputManager::get_profiles()) if (ValidName(name)) w.String(name.c_str());
	w.EndArray();
	w.Key("buttons"); w.StartArray();
	if (s.selected) for (unsigned id = 0; id < SDL_GAMEPAD_BUTTON_COUNT; ++id) { w.StartObject(); w.Key("id"); w.Uint(id); w.Key("name"); w.String(s.selected->get_button_name(id).c_str()); w.EndObject(); }
	w.EndArray();
	const auto pad = InputManager::instance().get_controller(0);
	w.Key("profile"); w.String(pad ? pad->get_profile_name().c_str() : "default");
	std::map<unsigned, unsigned> mappings;
	try
	{
		const auto doc = s.Current();
		for (const auto entry : doc.child("emulated_controller").child("controller").child("mappings").children("entry")) mappings.emplace(entry.child("mapping").text().as_uint(), entry.child("button").text().as_uint());
	}
	catch (...) {}
	const std::array<const char*, 27> labels{"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Plus", "Minus", "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right", "Left Stick Click", "Right Stick Click", "Left Stick Up", "Left Stick Down", "Left Stick Left", "Left Stick Right", "Right Stick Up", "Right Stick Down", "Right Stick Left", "Right Stick Right", "Mic", "Screen", "Home"};
	const SDLController labelDevice(SDL_GUID{}, 0);
	w.Key("mapping"); w.StartArray();
	for (unsigned id = 1; id < VPADController::kButtonId_Max; ++id)
	{
		w.StartObject(); w.Key("id"); w.Uint(id); w.Key("label"); w.String(labels[id - 1]);
		const auto it = mappings.find(id);
		w.Key("name"); w.String(it == mappings.end() ? "" : labelDevice.get_button_name(it->second).c_str());
		w.Key("button"); if (it == mappings.end()) w.Null(); else w.Uint(it->second);
		w.Key("duplicate"); w.Bool(it != mappings.end() && std::count_if(mappings.begin(), mappings.end(), [&](const auto& m) { return m.second == it->second; }) > 1);
		w.EndObject();
	}
	w.EndArray();
	const auto settings = s.selected ? s.selected->get_settings() : ControllerBase::Settings{};
	w.Key("leftDeadzone"); w.Double(settings.axis.deadzone); w.Key("rightDeadzone"); w.Double(settings.rotation.deadzone);
	w.EndObject(); return buffer.GetString();
}

std::string InputService::Command(std::string_view payload)
{
	auto& s = *m_impl;
	try
	{
		RequireInput(IsHealthy() && payload.size() <= 1024 && payload.find('\0') == std::string_view::npos);
		rapidjson::Document request; request.Parse(payload.data(), payload.size());
		RequireInput(!request.HasParseError() && request.IsObject() && request.HasMember("op") && request["op"].IsString());
		const std::string op(request["op"].GetString(), request["op"].GetStringLength());
		RequireInput(request.HasMember("revision") && request["revision"].IsUint64() && request["revision"].GetUint64() == s.revision);
		for (auto it = request.MemberBegin(); it != request.MemberEnd(); ++it)
		{
			const std::string_view key = it->name.GetString();
			RequireInput(key == "op" || key == "revision" || (op == "select" && key == "token") || (op == "capture" && (key == "mapping" || key == "continuous")) || ((op == "load" || op == "save-as") && key == "name") || (op == "deadzone" && (key == "left" || key == "right")));
		}
		const auto value = [&](const char* key) -> const rapidjson::Value& { RequireInput(request.HasMember(key)); return request[key]; };
		if (op == "cancel") { s.captureId = 0; s.notice = "capture-cancelled"; }
		else if (op == "capture")
		{
			RequireInput(value("mapping").IsUint() && (!request.HasMember("continuous") || value("continuous").IsBool()));
			s.Capture(value("mapping").GetUint(), request.HasMember("continuous") && value("continuous").GetBool());
		}
		else if (op == "skip")
		{
			RequireInput(s.captureId && s.continuous);
			if (s.advancing) { ++s.captureMapping; s.advancing = false; }
			if (s.captureMapping >= Impl::kBasicMappings) { s.captureId = 0; s.continuous = false; s.notice = "saved"; }
			else { ++s.captureMapping; s.advancing = false; s.neutral = false; s.notice = "release-inputs"; s.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10); }
		}
		else if (op == "finish") { RequireInput(s.captureId != 0); s.captureId = 0; s.continuous = s.advancing = false; s.notice = "saved"; }
		else
		{
			s.captureId = 0;
			auto doc = s.Current();
			if (op == "select")
			{
				RequireInput(value("token").IsUint64()); s.RefreshDevices();
				const auto it = std::find_if(s.devices.begin(), s.devices.end(), [&](const auto& d) { return d.token == value("token").GetUint64(); });
				RequireInput(it != s.devices.end() && it->controller->connect());
				s.Select(it->controller); s.autoSelect = true;
			}
			else if (op == "load")
			{
				RequireInput(value("name").IsString() && ValidName(std::string_view(value("name").GetString(), value("name").GetStringLength())));
				const std::string name = value("name").GetString(); const auto bytes = Read(s.directory / _utf8ToPath(name + ".xml"));
				RequireInput(doc.load_buffer(bytes.data(), bytes.size())); Validate(doc); Text(doc.child("emulated_controller"), "profile", name);
				RequireInput(s.selected && s.selected->is_connected());
				const auto c = doc.child("emulated_controller").child("controller");
				RequireInput(s.selected->uuid().substr(s.selected->uuid().find('_')) == std::string(c.child_value("uuid")).substr(std::string(c.child_value("uuid")).find('_')));
				Text(c, "uuid", s.selected->uuid()); Text(c, "launcher_serial", s.selected->serial()); s.Persist(doc, false);
			}
			else
			{
				Validate(doc); RequireInput(s.selected && s.selected->is_connected());
				if (op == "save-as")
				{
					RequireInput(value("name").IsString() && ValidName(std::string_view(value("name").GetString(), value("name").GetStringLength()))); Text(doc.child("emulated_controller"), "profile", value("name").GetString()); s.Persist(doc, true, true);
				}
				else if (op == "deadzone")
				{
					RequireInput(value("left").IsNumber() && value("right").IsNumber());
					for (const auto& [key, group] : {std::pair{"left", "axis"}, std::pair{"right", "rotation"}})
					{
						const auto v = value(key).GetDouble(); RequireInput(std::isfinite(v) && v >= 0 && v <= 0.5 && std::abs(v * 100 - std::round(v * 100)) < 0.00001);
						Text(doc.child("emulated_controller").child("controller").child(group), "deadzone", fmt::format("{:.2f}", v));
					}
					s.Persist(doc, true);
				}
				else if (op == "reset")
				{
					auto original = InputManager::instance().get_controller(0); auto defaults = std::make_shared<VPADController>(0);
					defaults->add_controller(s.selected); defaults->set_default_mapping(s.selected); InputManager::instance().set_controller(defaults);
					auto base = s.Current(); InputManager::instance().set_controller(original);
					auto c = doc.child("emulated_controller").child("controller"); c.remove_child("mappings"); c.append_copy(base.child("emulated_controller").child("controller").child("mappings"));
					for (const auto group : {"axis", "rotation"}) Text(c.child(group), "deadzone", "0.25"); s.Persist(doc, true);
				}
				else RequireInput(false);
			}
		}
	}
	catch (...) { s.captureId = 0; if (s.notice != "save-failed" && s.notice != "recovery-required") s.notice = "rejected"; }
	return Snapshot();
}

std::string InputService::Poll()
{
	auto& s = *m_impl;
	const auto raw = s.Raw();
	try { s.CaptureStep(raw); } catch (...) { s.captureId = 0; if (s.notice != "save-failed" && s.notice != "recovery-required") s.notice = "rejected"; }
	auto left = raw.axis, right = raw.rotation;
	if (s.selected)
	{
		const auto settings = s.selected->get_settings();
		s.selected->apply_axis_setting(left, {}, settings.axis); s.selected->apply_axis_setting(right, {}, settings.rotation);
	}
	rapidjson::StringBuffer buffer; Writer w(buffer); w.StartObject();
	w.Key("captureId"); w.Uint64(s.captureId); w.Key("notice"); w.String(s.notice.c_str());
	w.Key("captureMapping"); w.Uint(s.captureMapping); w.Key("continuous"); w.Bool(s.continuous); w.Key("advancing"); w.Bool(s.advancing);
	w.Key("revision"); w.Uint64(s.revision);
	w.Key("completed"); w.StartArray(); for (auto id : s.completed) w.Uint(id); w.EndArray();
	w.Key("connected"); w.Bool(s.selected && s.selected->is_connected());
	w.Key("rawLeft"); Vector(w, raw.axis); w.Key("rawRight"); Vector(w, raw.rotation);
	w.Key("left"); Vector(w, left); w.Key("right"); Vector(w, right); w.Key("triggers"); Vector(w, raw.trigger);
	auto pressed = raw.buttons.GetButtonList();
	for (const auto& [axis, positive, negative] : std::array<std::tuple<glm::vec2, unsigned, unsigned>, 3>{{{left, kAxisXP, kAxisXN}, {right, kRotationXP, kRotationXN}, {raw.trigger, kTriggerXP, kTriggerXN}}})
	{
		if (std::abs(axis.x) > ControllerState::kAxisThreshold) pressed.push_back(axis.x > 0 ? positive : negative);
		if (std::abs(axis.y) > ControllerState::kAxisThreshold) pressed.push_back(axis.y > 0 ? positive + 1 : negative + 1);
	}
	w.Key("buttons"); w.StartArray(); for (auto id : pressed) w.Uint(id); w.EndArray();
	w.EndObject(); return buffer.GetString();
}
