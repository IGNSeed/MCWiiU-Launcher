#include "UpdateService.h"
#include "UpdateIdentity.h"
#include "UpdateHandoff.h"
#include "LauncherPaths.h"
#include "VersionedInstallation.h"
#include <glaze/glaze.hpp>

using namespace LauncherUpdate;
namespace UpdateServiceDetail
{
	struct Asset { uint64_t id = 0; std::string name, browser_download_url, state; uint64_t size = 0; };
	struct Release { uint64_t id = 0; std::string tag_name; bool draft = true, prerelease = true; std::vector<Asset> assets; };
	struct Snapshot { std::string state, targetId, version, notes, error; uint64_t revision, downloaded, size; bool canCancel, blocksPlay; std::string packageKind, foundationVersion; };
	Candidate ReadCandidate(const Release& release, std::string_view releaseJson, std::atomic_bool& cancel, bool foundation = false)
	{
		Candidate candidate;
		for (auto name : (foundation ? std::array{"foundation-update-manifest.json", "foundation-update-manifest.sig"} : std::array{"update-manifest.json", "update-manifest.sig"}))
		{
			const Asset* selected = nullptr;
			for (const auto& asset : release.assets) if (asset.name == name) { Require(!selected && asset.id && asset.state == "uploaded", "duplicate-manifest-asset"); selected = &asset; }
			Require(selected && selected->size > 0 && selected->size <= (std::string_view(name).ends_with(".sig") ? 128U : 256 * 1024U), "missing-or-oversized-manifest");
			Require(selected->browser_download_url == "https://github.com/IGNSeed/MCWiiU-Launcher/releases/download/" + release.tag_name + "/" + name, "invalid-manifest-asset-url");
			const auto metadata = Fetch(selected->browser_download_url, static_cast<size_t>(selected->size), cancel);
			Require(metadata.body.size() == selected->size, "manifest-asset-size-mismatch");
			if (std::string_view(name).ends_with(".sig")) candidate.signature = metadata.body; else candidate.bytes = metadata.body;
		}
		candidate.manifest = VerifyManifest(candidate.bytes, candidate.signature, TrustedPublicKey, ProductId);
		Require(candidate.manifest.package_format == (foundation ? "inno-setup" : "zip"), "invalid-update-manifest-kind");
		BindRelease(candidate, releaseJson);
		return candidate;
	}

}
using namespace UpdateServiceDetail;

UpdateService::UpdateService(fs::path cache) : m_cache(std::move(cache) / L"Updates") {}
UpdateService::~UpdateService() { m_cancel = true; if (m_worker.joinable()) m_worker.join(); }
void UpdateService::Move(State state) { Require(CanTransition(m_state, state), "invalid-update-state-transition"); m_state = state; ++m_generation; }
void UpdateService::Start(std::function<void()> operation)
{
	if (m_worker.joinable()) m_worker.join();
	m_cancel = false; m_working = true;
	m_worker = std::thread([this, operation = std::move(operation)] {
		try { operation(); }
		catch (const std::exception& error)
		{
			std::lock_guard lock(m_mutex);
			if (m_cancel && CanCancel(m_state)) { Move(State::Idle); m_candidate = {}; }
			else { m_error = error.what(); if (CanTransition(m_state, State::Failed)) Move(State::Failed); }
		}
		std::lock_guard lock(m_mutex);
		if (m_cancel && CanCancel(m_state)) { Move(State::Idle); m_candidate = {}; }
		m_working = false;
	});
}
void UpdateService::Check()
{
	{
		std::lock_guard lock(m_mutex);
		if (m_working || !CanTransition(m_state, State::Checking)) return;
		Move(State::Checking); m_error.clear(); m_candidate = {}; m_downloaded = 0;
	}
	Start([this] { CheckWorker(); });
}
void UpdateService::CheckWorker()
{
	const auto response = Fetch("https://api.github.com/repos/IGNSeed/MCWiiU-Launcher/releases/latest", 512 * 1024, m_cancel);
	if (response.status == 404) { std::lock_guard lock(m_mutex); Require(!m_cancel, "cancelled"); Move(State::UpToDate); return; }
	Release release;
	Require(!glz::read<glz::opts{.error_on_unknown_keys = false}>(release, response.body) && release.id && !release.draft && !release.prerelease, "invalid-or-nonstable-release");
	Require(release.tag_name.starts_with("v"), "invalid-release-version");
	const auto version = Version::Parse(std::string_view(release.tag_name).substr(1));
	if (version <= Version::Parse(ExecutableVersion(ProcessImage()))) { std::lock_guard lock(m_mutex); Require(!m_cancel, "cancelled"); Move(State::UpToDate); return; }
	auto candidate = ReadCandidate(release, response.body, m_cancel);
	if (candidate.manifest.package_format == "zip") {
		bool compatible = false;
		try { compatible = FoundationCompatible(ProcessImage().parent_path().parent_path().parent_path(),candidate.manifest); }
		catch (const std::exception&) { compatible = Version::Parse(candidate.manifest.required_bootstrapper_version) <= Version::Parse(BootstrapperVersion) && candidate.manifest.required_updater_protocol_version <= ProtocolVersion; }
		if (!compatible) {
			try { candidate = ReadCandidate(release,response.body,m_cancel,true); }
			catch (const std::exception&) { throw std::runtime_error("foundation-update-required-no-verified-setup-available"); }
		}
	}
	std::lock_guard lock(m_mutex); Require(!m_cancel, "cancelled"); m_candidate = std::move(candidate); m_release = response.body; Move(State::UpdateAvailable);
}
void UpdateService::Approve(std::string_view targetId)
{
	std::lock_guard lock(m_mutex);
	if (m_state != State::UpdateAvailable || targetId != m_candidate.targetId || m_working) return;
 try { if (m_candidate.manifest.package_format == "zip" && !FoundationCompatible(InstalledRoot(ProcessImage()),m_candidate.manifest)) { m_error = "foundation-update-required"; ++m_generation; return; } }
 catch (const std::exception& error) { m_error = error.what(); ++m_generation; return; }
 Move(State::Scheduled);
}
void UpdateService::Cancel()
{
	std::lock_guard lock(m_mutex);
	if (!CanCancel(m_state)) return;
	if (m_working) m_cancel = true;
	else { Move(State::Idle); m_candidate = {}; m_downloaded = 0; }
}
void UpdateService::BeginWhenSafe(bool safe)
{
	{
		std::lock_guard lock(m_mutex); if (!safe || m_state != State::Scheduled || m_working) return;
		Move(State::Downloading);
	}
	Start([this] { DownloadWorker(); });
}
void UpdateService::ConfirmApproval()
{
	const auto response = Fetch("https://api.github.com/repos/IGNSeed/MCWiiU-Launcher/releases/latest", 512 * 1024, m_cancel);
	Require(response.status == 200, "approved-release-no-longer-available");
	Release release;
	Require(!glz::read<glz::opts{.error_on_unknown_keys = false}>(release, response.body) && release.id && !release.draft && !release.prerelease, "approved-release-changed");
	const auto current = ReadCandidate(release, response.body, m_cancel, m_candidate.manifest.package_format == "inno-setup");
	Require(current.targetId == m_candidate.targetId && current.notes == m_candidate.notes, "approved-update-changed-check-again");
}
void UpdateService::DownloadWorker()
{
 const auto root = InstalledRoot(ProcessImage());
 const auto current = ReadCurrent(root); auto verified = VerifyVersion(root,current.current);
 Require(verified.manifest.version == ExecutableVersion(ProcessImage()) && RollbackCompatible(verified.manifest,m_candidate.manifest), "update-data-or-version-incompatible");
 Require(!fs::exists(root/L"pending.json"), "recover-pending-update-first");
	ConfirmApproval();
	CheckOrdinaryPath(m_cache);
	if (!fs::exists(m_cache)) CreatePrivateDirectory(m_cache);
	m_transaction = m_cache / Wide(RandomId()); CreatePrivateDirectory(m_transaction);
	m_cacheLocks = std::make_unique<PathLocks>(m_transaction);
	ULARGE_INTEGER free{}; Require(GetDiskFreeSpaceExW(m_transaction.c_str(), &free, nullptr, nullptr) && free.QuadPart >= m_candidate.manifest.package_size * 3 + 64 * 1024 * 1024, "insufficient-update-disk-space");
	const auto part = m_transaction / L"package.partial";
	{
		File output(CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)); Require(output.handle != INVALID_HANDLE_VALUE, "cannot-create-update-download");
		Fetch(m_candidate.url, static_cast<size_t>(m_candidate.manifest.package_size), m_cancel, output.handle, [this](uint64_t value) { std::lock_guard lock(m_mutex); m_downloaded = value; });
		Require(FlushFileBuffers(output.handle), "cannot-flush-update-download");
	}
	{ std::lock_guard lock(m_mutex); Require(!m_cancel, "cancelled"); Move(State::Verifying); }
	const auto manifest = VerifyManifest(m_candidate.bytes, m_candidate.signature, TrustedPublicKey, ProductId);
	VerifyPackage(part, manifest); Require(!m_cancel, "cancelled");
	ConfirmApproval();
	Require(MoveFileExW(part.c_str(), (m_transaction / Wide(manifest.package_filename)).c_str(), MOVEFILE_WRITE_THROUGH), "cannot-finalize-update-download");
	WriteNew(m_transaction / L"manifest.json", m_candidate.bytes); WriteNew(m_transaction / L"manifest.sig", m_candidate.signature);
 { std::lock_guard lock(m_mutex); Require(!m_cancel,"cancelled"); Move(State::Staging); }
 { auto operation = LockInstallation(root);
   if (manifest.package_format == "zip") StageVersion(root,m_transaction/Wide(manifest.package_filename),m_candidate.bytes,m_candidate.signature);
   else VerifyFoundationSetup(m_transaction/Wide(manifest.package_filename),manifest);
 }
	std::lock_guard lock(m_mutex); Require(!m_cancel, "cancelled"); Move(State::Staged);
}
void UpdateService::Install(std::string_view targetId)
{
	{
		std::lock_guard lock(m_mutex); if (m_working || m_state != State::Staged || targetId != m_candidate.targetId) return;
		Move(State::AwaitingShutdown);
	}
	Start([this] {
  Handoff handoff; handoff.manifest = m_candidate.bytes; handoff.signature = m_candidate.signature; handoff.release = m_release; handoff.targetId = m_candidate.targetId; handoff.nonce = RandomId();
		auto updater = StartUpdater(m_transaction, handoff);
		std::lock_guard lock(m_mutex); m_updater = std::move(updater); m_close = true;
	});
}
bool UpdateService::BlocksPlay() const { std::lock_guard lock(m_mutex); return m_state >= State::Downloading && m_state <= State::AwaitingShutdown; }
bool UpdateService::WantsSafeStop() const { std::lock_guard lock(m_mutex); return m_state == State::Scheduled && !m_working; }
bool UpdateService::WantsClose() const { std::lock_guard lock(m_mutex); return m_close; }
std::string UpdateService::Json() const
{
	std::lock_guard lock(m_mutex);
	return glz::write_json(Snapshot{std::string(Name(m_state)), m_candidate.targetId, m_candidate.manifest.version, m_candidate.notes, m_error, m_generation, m_downloaded, m_candidate.manifest.package_size, CanCancel(m_state), m_state >= State::Downloading && m_state <= State::AwaitingShutdown, m_candidate.manifest.package_format, m_candidate.manifest.required_bootstrapper_version}).value();
}
