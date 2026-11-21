#pragma once

#include <Windows.h>
#include <array>
#include <atomic>
#include <compare>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace LauncherUpdate
{
namespace fs = std::filesystem;
struct Version
{
	unsigned major = 0, minor = 0, patch = 0;
	auto operator<=>(const Version&) const = default;
	static Version Parse(std::string_view text);
};
enum class State { Idle, Checking, UpToDate, UpdateAvailable, Scheduled, Downloading, Verifying, Staging, Staged, AwaitingShutdown, Activating, VerifyingStartup, Committing, Completed, RollingBack, Failed, RecoveryRequired };
std::string_view Name(State state);
bool CanTransition(State from, State to);
bool CanCancel(State state);
struct Readiness
{
	bool titleStopped = false, coreSafe = false, rendererStopped = false;
	bool historySaved = false, settingsSaved = false, canClose = false;
	constexpr bool CanBegin() const
	{
		return titleStopped && coreSafe && rendererStopped && historySaved && settingsSaved && canClose;
	}
};
struct Compatibility
{
	unsigned preferences_schema = 2, history_schema = 1, account_schema = 1, game_data_schema = 1;
	bool rollback_safe = false;
	bool operator==(const Compatibility&) const = default;
};
struct Manifest
{
	unsigned schema_version = 2;
	std::string product_id, channel, version, architecture, release_tag;
	uint64_t release_asset_id = 0;
	std::string package_filename;
	uint64_t package_size = 0;
	std::string package_sha256, package_format, required_bootstrapper_version;
	unsigned required_updater_protocol_version = 2;
	Compatibility compatibility;
	struct Entry { std::string path; uint64_t size = 0; std::string sha256; };
	std::vector<Entry> files;
};
struct Candidate
{
	Manifest manifest;
	uint64_t releaseId = 0;
	std::string bytes, signature, targetId, notes, url;
};
bool RollbackCompatible(const Manifest& previous, const Manifest& next);
void Require(bool condition, const char* message);
std::wstring Wide(std::string_view value);
std::string Utf8(std::wstring_view value);
std::string Sha256(std::string_view bytes);
std::string FileSha256(HANDLE file);
std::string RandomId();
void VerifySignature(std::string_view bytes, std::string_view signature, std::string_view key);
Manifest VerifyManifest(std::string_view bytes, std::string_view signature, std::string_view key,
	std::string_view product);
std::string_view LauncherExecutableName(const Manifest& manifest);
void ValidatePackageName(std::string_view name);
void VerifyPackage(const fs::path& path, const Manifest& manifest);
void BindRelease(Candidate& candidate, std::string_view releaseJson);
void CheckOrdinaryPath(const fs::path& path);
void ValidateInstallPath(const fs::path& path, const fs::path& dataRoot);
void CreatePrivateDirectory(const fs::path& path);
struct File
{
	HANDLE handle = INVALID_HANDLE_VALUE;
	File() = default;
	explicit File(HANDLE value) : handle(value) {}
	~File();
	File(const File&) = delete;
	File& operator=(const File&) = delete;
	File(File&& other) noexcept;
	File& operator=(File&& other) noexcept;
};
File OpenLocked(const fs::path& path, bool executable = false);
class PathLocks final
{
public:
	explicit PathLocks(const fs::path& path);
private:
	std::vector<File> m_directories;
};
std::string ReadFileBytes(const fs::path& path, size_t limit);
void WriteNew(const fs::path& path, std::string_view bytes);
std::string ExecutableVersion(const fs::path& executable);
std::string ExecutableProductName(const fs::path& executable);
struct HttpResponse { unsigned status = 0; std::string body; };
using Progress = std::function<void(uint64_t)>;
HttpResponse Fetch(std::string url, size_t limit, std::atomic_bool& cancel,
	HANDLE output = INVALID_HANDLE_VALUE, Progress progress = {});
bool AllowedUrl(std::string_view url, bool cdn = false);
} // namespace LauncherUpdate
