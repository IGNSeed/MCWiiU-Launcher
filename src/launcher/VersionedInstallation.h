#pragma once
#include "UpdateSecurity.h"

namespace LauncherUpdate
{
	struct VersionRef
	{
		std::string version, package_id;
		bool operator==(const VersionRef&) const = default;
	};
	struct CurrentVersion
	{
		unsigned schema_version = 1;
		std::string product_id;
		VersionRef current, previous;
	};
	struct PendingVersion
	{
		unsigned schema_version = 1, updater_protocol_version = 2;
		std::string product_id, transaction_id, stage;
		VersionRef candidate, previous, prior_previous;
		unsigned recovery_attempts = 0;
	};
	struct VerifiedVersion
	{
		VersionRef reference;
		Manifest manifest;
		fs::path directory;
		std::vector<File> locks;
	};
	fs::path ProcessImage(HANDLE process = GetCurrentProcess());
	fs::path VersionDirectory(const fs::path& root, const VersionRef& reference);
	fs::path InstalledRoot(const fs::path& launcher);
	enum class VerificationPurpose
	{
		Runtime,
		SetupRepair
	};
	void ValidateRegisteredRoot(const fs::path& root, VerificationPurpose purpose = VerificationPurpose::Runtime);
	void ValidateSetupDestination(const fs::path& root);
	void EnsureProgramsStopped(const fs::path& root);
	void AtomicWrite(const fs::path& path, std::string_view bytes);
	CurrentVersion ReadCurrent(const fs::path& root);
	PendingVersion ReadPending(const fs::path& root);
	VerifiedVersion VerifyVersion(const fs::path& root, const VersionRef& reference, VerificationPurpose purpose = VerificationPurpose::Runtime);
	VersionRef StageVersion(const fs::path& root, const fs::path& package, std::string_view manifest, std::string_view signature);
	void WritePending(const fs::path& root, const PendingVersion& pending);
	void CommitVersion(const fs::path& root, const CurrentVersion& current);
	void ClearPending(const fs::path& root);
	void CleanupVersions(const fs::path& root, const CurrentVersion& current);
	void InitializeInstallation(const fs::path& root, const fs::path& source);
	void RemoveOwnedVersions(const fs::path& root);
	File LockInstallation(const fs::path& root);
	void VerifyFoundationFiles(const fs::path& root);
	bool FoundationCompatible(const fs::path& root, const Manifest& manifest);
	void VerifyFoundationSetup(const fs::path& setup, const Manifest& manifest);
} // namespace LauncherUpdate
