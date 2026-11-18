#include "GameDataBridge.h"

#include <Windows.h>
#include <objbase.h>
#include <algorithm>
#include <chrono>
#include <map>
#include <glaze/glaze.hpp>

namespace GameDataModel
{
	using Service = GameInstallationService;
	using namespace std::chrono_literals;
	struct Command
	{
		std::string op, operation, scope, part, targetRegion, dlcTitleId;
		std::vector<std::string> selectionIds;
	};
	struct DisplayTitle
	{
		std::string id, type, region, name;
		uint16_t version = 0;
		uint64_t bytes = 0;
	};
	struct Choice
	{
		std::string part, selectionId;
		DisplayTitle title;
	};
	struct DisplayProgress
	{
		std::string operation, phase, titleId, completion, error, message;
		uint64_t processedBytes = 0, totalBytes = 0;
		double percent = 0;
		bool cancellable = false;
	};
	struct Response
	{
		uint64_t requestId = 0;
		std::string error, message, state, region, scope, mode, targetRegion, operationId, dlcTitleId, stateError, stateMessage;
		bool ready = false, launchSupported = false, nativeAvailable = false, canManage = false, canSetup = false;
		bool busy = false, selectionPending = false, pickerCancelled = false;
		std::vector<DisplayTitle> titles;
		std::vector<Choice> choices;
		DisplayProgress progress;
	};
	std::string Hex(uint64_t value)
	{
		char text[17]{};
		for (size_t i = 0; i < 16; ++i) { text[15 - i] = "0123456789abcdef"[value & 15]; value >>= 4; }
		return text;
	}
	std::string NewId()
	{
		GUID guid{};
		if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Unable to create selection ID");
		wchar_t text[40]{};
		StringFromGUID2(guid, text, 40);
		std::string result;
		for (const auto* current = text; *current; ++current) result.push_back(static_cast<char>(*current));
		return result;
	}
	const char* Region(Service::Region value)
	{
		switch (value) { case Service::Region::JPN: return "JPN"; case Service::Region::USA: return "USA"; case Service::Region::EUR: return "EUR"; default: return "None"; }
	}
	const char* Type(Service::TitleType value)
	{
		switch (value) { case Service::TitleType::Base: return "base"; case Service::TitleType::Update: return "update"; default: return "dlc"; }
	}
	const char* Error(Service::Error value)
	{
		switch (value)
		{
#define GAME_DATA_ERROR(name) case Service::Error::name: return #name
			GAME_DATA_ERROR(None); GAME_DATA_ERROR(Busy); GAME_DATA_ERROR(GameActive); GAME_DATA_ERROR(InvalidPath);
			GAME_DATA_ERROR(UnsafePath); GAME_DATA_ERROR(InvalidMetadata); GAME_DATA_ERROR(UnsupportedTitle); GAME_DATA_ERROR(UnsupportedVersion);
			GAME_DATA_ERROR(RegionMismatch); GAME_DATA_ERROR(AlreadyInstalled); GAME_DATA_ERROR(NotInstalled); GAME_DATA_ERROR(UnmanagedTitle);
			GAME_DATA_ERROR(NoSpace); GAME_DATA_ERROR(AccessDenied); GAME_DATA_ERROR(CopyFailed); GAME_DATA_ERROR(Cancelled);
			GAME_DATA_ERROR(InvalidJournal); GAME_DATA_ERROR(RecoveryRequired); GAME_DATA_ERROR(IO);
#undef GAME_DATA_ERROR
		}
		return "IO";
	}
	const char* Phase(Service::Phase value)
	{
		switch (value)
		{
			case Service::Phase::Idle: return "Idle"; case Service::Phase::Analyzing: return "Analyzing";
			case Service::Phase::Planning: return "Planning"; case Service::Phase::Copying: return "Copying";
			case Service::Phase::Placing: return "Placing"; case Service::Phase::RollingBack: return "RollingBack";
			case Service::Phase::Cleaning: return "Cleaning"; case Service::Phase::Completed: return "Completed";
		}
		return "Idle";
	}
	const char* Completion(Service::Completion value)
	{
		switch (value) { case Service::Completion::Running: return "Running"; case Service::Completion::Succeeded: return "Succeeded"; case Service::Completion::Failed: return "Failed"; case Service::Completion::Cancelled: return "Cancelled"; default: return "None"; }
	}
	DisplayTitle Display(const Service::Title& title)
	{
		return {Hex(title.id), Type(title.type), Region(title.region), title.name, title.version, title.bytes};
	}
	std::string Fingerprint(const Service::Snapshot& snapshot)
	{
		std::string result = Service::StateName(snapshot.state);
		for (const auto& title : snapshot.titles) result += Hex(title.id) + ":" + std::to_string(title.version) + ";";
		return result;
	}
	bool Running(const Service::Snapshot& snapshot) { return snapshot.progress.completion == Service::Completion::Running; }
} // namespace GameDataModel

using namespace GameDataModel;

struct GameDataBridge::Impl
{
	Callbacks callbacks;
	bool nativeAvailable = false, closing = false, pickerActive = false;
	uint64_t lastRequest = 0;
	std::string scope, mode, targetRegion, fingerprint, operationId;
	uint64_t dlcTitleId = 0;
	std::chrono::steady_clock::time_point expires;
	struct Selection { std::string id; Service::Title title; };
	std::map<std::string, Selection> selections;
	std::future<Service::Analysis> analysis;
	std::string analysisScope, analysisPart;
	Service::Result selectionResult;

	bool CanManage() const
	{
		const auto snapshot = callbacks.snapshot();
		return !closing && !pickerActive && !Running(snapshot) && snapshot.state != Service::State::RecoveryRequired &&
			snapshot.state != Service::State::Error && callbacks.canManage();
	}
	void Clear()
	{
		scope.clear(); mode.clear(); targetRegion.clear(); selections.clear(); selectionResult = {}; dlcTitleId = 0;
	}
	bool ValidScope(const std::string& requested) const
	{
		return !scope.empty() && requested == scope && std::chrono::steady_clock::now() < expires && fingerprint == Fingerprint(callbacks.snapshot());
	}
	Service::Result Validate(const std::string& part, const Service::Title& title) const
	{
		const auto snapshot = callbacks.snapshot();
		if (part != Type(title.type)) return {Service::Error::UnsupportedTitle, "Select the requested title type"};
		if (part == "update" && title.version != 560) return {Service::Error::UnsupportedVersion, "Only update v560 is supported"};
		const auto region = targetRegion.empty() ? Region(snapshot.region) : targetRegion.c_str();
		if (part != "dlc" && std::string_view(region) != "None" && region != std::string(Region(title.region)))
			return {Service::Error::RegionMismatch, "Select the matching BASE and Update region"};
		if (part == "update")
		{
			const auto base = selections.find("base");
			if (base != selections.end() && base->second.title.region != title.region)
				return {Service::Error::RegionMismatch, "BASE and Update regions disagree"};
		}
		const bool installed = std::ranges::any_of(snapshot.titles, [&](const auto& item) { return item.id == title.id; });
		const bool reinstall = mode == "ReinstallBase" || mode == "ReinstallUpdate" ||
			(mode == "Setup" && part == "update" && installed);
		if (reinstall && !installed) return {Service::Error::NotInstalled, "Select the installed title for reinstallation"};
		if (!reinstall && mode != "ChangeRegion" && installed) return {Service::Error::AlreadyInstalled, "Use an explicit reinstallation operation"};
		return {};
	}
	bool AllowsPart(const std::string& part) const
	{
		if (mode == "Setup")
		{
			const auto snapshot = callbacks.snapshot();
			const auto installed = std::ranges::find_if(snapshot.titles, [&](const auto& item) { return part == Type(item.type); });
			return (part == "base" && installed == snapshot.titles.end()) ||
				(part == "update" && (installed == snapshot.titles.end() || installed->version != 560)) || part == "dlc";
		}
		return (mode == "ChangeRegion" && (part == "base" || part == "update")) ||
			((mode == "InstallUpdate" || mode == "ReinstallUpdate") && part == "update") ||
			(mode == "ReinstallBase" && part == "base") || (mode == "InstallDLC" && part == "dlc");
	}
	void Collect()
	{
		if (!scope.empty() && std::chrono::steady_clock::now() >= expires)
		{
			Clear(); selectionResult = {Service::Error::InvalidMetadata, "Selection expired; reopen setup and select the sources again"};
		}
		if (!analysis.valid() || analysis.wait_for(0s) != std::future_status::ready) return;
		Service::Analysis result;
		try { result = analysis.get(); }
		catch (...) { result.result = {Service::Error::IO, "Cannot analyze the selected source"}; }
		if (closing || !ValidScope(analysisScope) || !CanManage()) return;
		selectionResult = result.result;
		if (selectionResult) selectionResult = Validate(analysisPart, result.title);
		if (selectionResult)
		{
			selections[analysisPart] = {NewId(), std::move(result.title)};
			if (analysisPart == "base") selections.erase("update");
		}
	}
	Response Snapshot(uint64_t requestId)
	{
		Collect();
		const auto snapshot = callbacks.snapshot();
		const auto& progress = snapshot.progress;
		Response result;
		result.requestId = requestId; result.state = Service::StateName(snapshot.state); result.region = Region(snapshot.region);
		result.dlcTitleId = Hex(dlcTitleId); result.scope = scope; result.mode = mode; result.targetRegion = targetRegion; result.operationId = operationId;
		result.ready = snapshot.ready; result.launchSupported = snapshot.launchSupported; result.nativeAvailable = nativeAvailable;
		result.selectionPending = analysis.valid() || pickerActive; result.busy = result.selectionPending || Running(snapshot);
		result.canManage = CanManage() && !result.selectionPending;
		result.canSetup = result.canManage && !snapshot.ready;
		result.error = Error(selectionResult.error); result.message = selectionResult.message;
		result.stateError = Error(snapshot.result.error); result.stateMessage = snapshot.result.message;
		for (const auto& title : snapshot.titles) result.titles.push_back(Display(title));
		for (const auto& [part, selection] : selections) result.choices.push_back({part, selection.id, Display(selection.title)});
		result.progress = {mode, Phase(progress.phase), Hex(progress.titleId), Completion(progress.completion), Error(progress.result.error), progress.result.message,
			progress.processedBytes, progress.totalBytes, progress.percent, progress.cancellable};
		return result;
	}
	Service::Result Begin(const Command& command)
	{
		if (!CanManage() || analysis.valid()) return {Service::Error::Busy, "Stop the game and other operations first"};
		const auto snapshot = callbacks.snapshot();
		const auto has = [&](Service::TitleType type) { return std::ranges::any_of(snapshot.titles, [&](const auto& item) { return item.type == type; }); };
		const auto& value = command.operation;
		if (value != "Setup" && value != "InstallUpdate" && value != "InstallDLC" && value != "ReinstallBase" &&
			value != "ReinstallUpdate" && value != "ChangeRegion" && value != "RemoveDLC" && value != "Uninstall")
			return {Service::Error::InvalidMetadata, "Unknown game data operation"};
		if ((value == "Setup" && snapshot.ready) || ((value == "InstallUpdate" || value == "ChangeRegion" || value == "ReinstallBase") && !has(Service::TitleType::Base)) ||
			(value == "InstallUpdate" && has(Service::TitleType::Update)) || (value == "ReinstallUpdate" && !has(Service::TitleType::Update)) ||
			(value == "Uninstall" && snapshot.titles.empty())) return {Service::Error::NotInstalled, "Operation is unavailable for the current installation"};
		uint64_t targetDlc = 0;
		if (value == "RemoveDLC")
		{
			for (const auto& title : snapshot.titles) if (title.type == Service::TitleType::DLC && Hex(title.id) == command.dlcTitleId) targetDlc = title.id;
			if (!targetDlc) return {Service::Error::NotInstalled, "Select an installed DLC title"};
		}
		if (value == "ChangeRegion" && ((command.targetRegion != "JPN" && command.targetRegion != "USA" && command.targetRegion != "EUR") || command.targetRegion == Region(snapshot.region)))
			return {Service::Error::RegionMismatch, "Select a different region"};
		Clear(); scope = NewId(); mode = value; dlcTitleId = targetDlc;
		targetRegion = value == "ChangeRegion" ? command.targetRegion : "";
		fingerprint = Fingerprint(snapshot); expires = std::chrono::steady_clock::now() + 30min;
		return {};
	}
	Service::Result Start(const Command& command)
	{
		if (!CanManage() || analysis.valid()) return {Service::Error::Busy, "Operation is busy"};
		if (!ValidScope(command.scope)) return {Service::Error::InvalidMetadata, "Selection expired or installation changed; select the sources again"};
		if (command.selectionIds.size() != selections.size()) return {Service::Error::InvalidMetadata, "Selection IDs do not match this operation"};
		std::vector<std::string> ids = command.selectionIds;
		std::ranges::sort(ids);
		if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return {Service::Error::InvalidMetadata, "Duplicate selection ID"};
		Service::Request request;
		request.dlcTitleId = dlcTitleId;
		request.operation = mode == "ReinstallBase" ? Service::Operation::ReinstallBase : mode == "ReinstallUpdate" ? Service::Operation::ReinstallUpdate :
			mode == "ChangeRegion" ? Service::Operation::ChangeRegion : mode == "Uninstall" ? Service::Operation::Uninstall : mode == "RemoveDLC" ? Service::Operation::RemoveDLC : Service::Operation::Install;
		const auto snapshot = callbacks.snapshot();
		for (const auto& [part, selection] : selections)
		{
			if (std::ranges::find(ids, selection.id) == ids.end()) return {Service::Error::InvalidMetadata, "Stale selection ID"};
			if (const auto checked = Validate(part, selection.title); !checked) return checked;
			request.sources.push_back(selection.title.source);
			request.expectedSources.push_back({selection.title.id, selection.title.version});
		}
		if (mode == "Setup")
		{
			const bool base = selections.contains("base") || std::ranges::any_of(snapshot.titles, [](const auto& title) { return title.type == Service::TitleType::Base; });
			const bool update = selections.contains("update") || std::ranges::any_of(snapshot.titles, [](const auto& title) { return title.type == Service::TitleType::Update && title.version == 560; });
			if (!base || !update) return {Service::Error::NotInstalled, "Select BASE and matching Update v560"};
			const bool replaceUpdate = std::ranges::any_of(snapshot.titles, [](const auto& title) { return title.type == Service::TitleType::Update && title.version != 560; });
			if (replaceUpdate)
			{
				if (selections.size() != 1 || !selections.contains("update")) return {Service::Error::InvalidMetadata, "Reinstall Update first; add DLC afterwards"};
				request.operation = Service::Operation::ReinstallUpdate;
			}
		}
		if (mode == "ChangeRegion" && (selections.size() != 2 || !selections.contains("base") || !selections.contains("update")))
			return {Service::Error::NotInstalled, "Select the new BASE and matching Update v560"};
		const auto accepted = callbacks.start(std::move(request));
		if (accepted) { operationId = NewId(); scope.clear(); selections.clear(); selectionResult = {}; }
		return accepted;
	}
};

GameDataBridge::GameDataBridge(Callbacks callbacks, bool nativeAvailable) : m_impl(std::make_unique<Impl>())
{
	m_impl->callbacks = std::move(callbacks); m_impl->nativeAvailable = nativeAvailable;
}
GameDataBridge::~GameDataBridge() = default;

std::string GameDataBridge::Handle(uint64_t requestId, std::string_view payload, const Picker& picker)
{
	auto& state = *m_impl;
	state.Collect();
	Service::Result result;
	Command command;
	bool cancelled = false;
	if (!requestId || requestId <= state.lastRequest) result = {Service::Error::InvalidMetadata, "Stale request ID"};
	else
	{
		try
		{
			state.lastRequest = requestId;
			if (payload.empty() || payload.size() > 4096 || payload.find('\0') != std::string_view::npos || glz::read_json(command, payload))
				result = {Service::Error::InvalidMetadata, "Invalid game data command"};
			else if (command.op == "get") {}
			else if (command.op == "begin") result = state.Begin(command);
			else if (command.op == "discard")
			{
				if (state.scope != command.scope || state.pickerActive) result = {Service::Error::Busy, "Selection session changed"};
				else state.Clear();
			}
			else if (command.op == "clear")
			{
				if (!state.ValidScope(command.scope) || state.analysis.valid() || state.pickerActive) result = {Service::Error::Busy, "Selection is busy or expired"};
				else { state.selections.erase(command.part); state.selectionResult = {}; }
			}
			else if (command.op == "pick-folder" || command.op == "pick-meta")
			{
				if (!state.CanManage() || state.analysis.valid()) result = {Service::Error::Busy, "Operation is busy"};
				else if (!state.ValidScope(command.scope) || !state.AllowsPart(command.part)) result = {Service::Error::InvalidMetadata, "Invalid selection session or title type"};
				else
				{
					state.pickerActive = true;
					std::optional<Service::Path> selected;
					try { selected = picker(command.op == "pick-folder"); }
					catch (...) { result = {Service::Error::IO, "Cannot open the Windows selection dialog"}; }
					state.pickerActive = false;
					cancelled = !selected && bool(result);
					if (selected && state.ValidScope(command.scope) && state.CanManage())
					{
						state.selections.erase(command.part);
						if (command.part == "base") state.selections.erase("update");
						state.selectionResult = {}; state.analysisScope = state.scope; state.analysisPart = command.part;
						state.analysis = state.callbacks.analyze(std::move(*selected));
					}
				}
			}
			else if (command.op == "start") result = state.Start(command);
			else if (command.op == "cancel")
			{
				if (command.scope != state.operationId || !state.callbacks.snapshot().progress.cancellable) result = {Service::Error::Busy, "The current phase cannot be cancelled"};
				else state.callbacks.cancel();
			}
			else result = {Service::Error::InvalidMetadata, "Unknown game data command"};
		}
		catch (...) { result = {Service::Error::IO, "Cannot process game data command; existing data was preserved"}; }
	}
	auto response = state.Snapshot(requestId);
	if (!result) { response.error = Error(result.error); response.message = result.message; }
	response.pickerCancelled = cancelled;
	return glz::write_json(response).value();
}

bool GameDataBridge::IsWorking()
{
	m_impl->Collect();
	return m_impl->pickerActive || m_impl->analysis.valid() || Running(m_impl->callbacks.snapshot());
}
bool GameDataBridge::IsCancellable() { return m_impl->callbacks.snapshot().progress.cancellable; }
void GameDataBridge::BeginClose()
{
	m_impl->closing = true; m_impl->Clear();
	if (IsCancellable()) m_impl->callbacks.cancel();
}
