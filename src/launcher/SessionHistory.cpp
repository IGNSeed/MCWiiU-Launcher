#include "SessionHistory.h"

#include <algorithm>
#include <objbase.h>
#include <vector>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <glaze/glaze.hpp>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace SessionHistoryDetail
{
	using namespace std::chrono;
	constexpr uint64_t Second = 1000000, Day = 86400 * Second;
	constexpr uint64_t MaxDuration = 9007199254740991ULL;
	constexpr size_t MaxDays = 146400, MaxBytes = 16 * 1024 * 1024;
	constexpr auto CheckpointInterval = seconds(45);
	using Days = std::map<std::string, uint64_t>;

	void Require(bool value, const char* message)
	{
		if (!value) throw std::runtime_error(message);
	}
	struct File
	{
		HANDLE value = INVALID_HANDLE_VALUE;
		~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
	};
	void OrdinaryFile(HANDLE handle)
	{
		BY_HANDLE_FILE_INFORMATION info{};
		Require(GetFileInformationByHandle(handle, &info) && GetFileType(handle) == FILE_TYPE_DISK &&
			!(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) && info.nNumberOfLinks == 1,
			"unsafe-file");
	}
	std::optional<std::string> Read(const LauncherPaths& paths, const std::filesystem::path& path)
	{
		paths.CheckPath(path);
		File file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
		if (file.value == INVALID_HANDLE_VALUE)
		{
			Require(GetLastError() == ERROR_FILE_NOT_FOUND, "read-failed");
			return std::nullopt;
		}
		OrdinaryFile(file.value);
		LARGE_INTEGER size{};
		Require(GetFileSizeEx(file.value, &size) && size.QuadPart > 0 && size.QuadPart <= MaxBytes, "invalid-size");
		std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
		DWORD read = 0;
		Require(ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size(), "read-failed");
		return bytes;
	}
	std::string Date(sys_days value)
	{
		const year_month_day date(value);
		char text[16];
		sprintf_s(text, "%04d-%02u-%02u", int(date.year()), unsigned(date.month()), unsigned(date.day()));
		return text;
	}
	std::optional<sys_days> ParseDate(std::string_view text)
	{
		if (text.size() != 10 || text[4] != '-' || text[7] != '-') return std::nullopt;
		int values[3]{}, index = 0;
		for (size_t i = 0; i < text.size(); ++i)
		{
			if (i == 4 || i == 7) { ++index; continue; }
			if (text[i] < '0' || text[i] > '9') return std::nullopt;
			values[index] = values[index] * 10 + text[i] - '0';
		}
		const year_month_day date{year(values[0]), month(values[1]), day(values[2])};
		if (values[0] < 1601 || values[0] > 9999 || !date.ok()) return std::nullopt;
		return sys_days(date);
	}
	std::string LocalDate(uint64_t utcUs, const DYNAMIC_TIME_ZONE_INFORMATION& zone)
	{
		const ULARGE_INTEGER value{.QuadPart = utcUs * 10};
		const FILETIME time{value.LowPart, value.HighPart};
		SYSTEMTIME utc{}, local{};
		Require(FileTimeToSystemTime(&time, &utc) && SystemTimeToTzSpecificLocalTimeEx(&zone, &utc, &local), "calendar-failed");
		return Date(sys_days(year(local.wYear) / month(local.wMonth) / day(local.wDay)));
	}
	struct Checkpoint
	{
		std::string id;
		uint64_t sessionId = 0, sequence = 0, startedUtcUs = 0, sampledUtcUs = 0, durationUs = 0;
		bool active = false;
	};
	struct Ledger
	{
		uint32_t schema = 1;
		uint64_t revision = 0, totalUs = 0;
		Days days;
		Checkpoint checkpoint;
	};
	bool Extends(const Ledger& previous, const Ledger& current)
	{
		if (previous.revision > current.revision || previous.totalUs > current.totalUs) return false;
		for (const auto& [date, duration] : previous.days)
		{
			const auto found = current.days.find(date);
			if (found == current.days.end() || found->second < duration) return false;
		}
		return true;
	}
	bool Valid(const Ledger& ledger)
	{
		if (ledger.schema != 1 || ledger.totalUs > MaxDuration || ledger.days.size() > MaxDays || !ledger.revision || ledger.revision > MaxDuration) return false;
		uint64_t total = 0;
		for (const auto& [date, duration] : ledger.days)
		{
			if (!ParseDate(date) || !duration || duration > MaxDuration - total) return false;
			total += duration;
		}
		const auto& point = ledger.checkpoint;
		return total == ledger.totalUs && point.id.size() == 36 &&
			point.id.find_first_not_of("0123456789abcdefABCDEF-") == std::string::npos &&
			point.sessionId && point.sequence && point.sequence <= ledger.revision && point.durationUs <= total &&
			point.startedUtcUs && point.sampledUtcUs && point.startedUtcUs <= 265046774399999999ULL &&
			point.sampledUtcUs <= 265046774399999999ULL;
	}
	Ledger Decode(std::string_view bytes)
	{
		Ledger ledger;
		Require(!glz::read<glz::opts{.error_on_missing_keys = true}>(ledger, bytes) && Valid(ledger) && glz::write_json(ledger).value() == bytes, "invalid-history");
		return ledger;
	}
	std::string NewId()
	{
		GUID id{};
		Require(SUCCEEDED(CoCreateGuid(&id)), "identity-failed");
		char text[40];
		sprintf_s(text, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", id.Data1, id.Data2, id.Data3,
			id.Data4[0], id.Data4[1], id.Data4[2], id.Data4[3], id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7]);
		return text;
	}
	uint64_t Sum(const Days& dates, sys_days begin, sys_days end)
	{
		uint64_t total = 0;
		const auto endDate = Date(end);
		for (auto item = dates.lower_bound(Date(begin)); item != dates.end() && item->first < endDate; ++item) total += item->second;
		return total;
	}
	struct Request
	{
		std::string mode = "daily", anchor;
		int year = 0, month = 0;
	};
	struct Bucket
	{
		std::string start, end;
		uint64_t seconds = 0;
	};
	struct View
	{
		std::string notice, today, mode;
		bool active = false, recovered = false, readOnly = false;
		uint64_t revision = 0, totalSeconds = 0, savedSeconds = 0, sessionSeconds = 0;
		uint64_t todaySeconds = 0, weekSeconds = 0, monthSeconds = 0, yearSeconds = 0;
		int firstYear = 0, lastYear = 0, year = 0, month = 0;
		std::vector<Bucket> buckets, dates;
	};
} // namespace SessionHistoryDetail

using namespace SessionHistoryDetail;

struct SessionHistory::Impl
{
	const LauncherPaths& paths;
	const std::filesystem::path file, backup, pending;
	Clock clock;
	File ownership;
	mutable std::mutex mutex;
	std::condition_variable wake;
	std::thread worker;
	Ledger ledger;
	ClockSample previous;
	uint64_t seenSession = 0, savedRevision = 0, savedTotal = 0;
	bool active = false, recovered = false, readOnly = false, stopping = false, urgent = false;
	std::string notice, durable;

	Impl(const LauncherPaths& storage, Clock source)
		: paths(storage), file(paths.data / L"session-history.json"), backup(paths.data / L"session-history.backup.json"),
		pending(paths.data / L"session-history.pending"), clock(std::move(source))
	{
		try
		{
			const auto lock = paths.data / L"session-history.lock";
			paths.CheckPath(lock);
			ownership.value = CreateFileW(lock.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
			Require(ownership.value != INVALID_HANDLE_VALUE, "history-in-use");
			OrdinaryFile(ownership.value);
			const auto bytes = Read(paths, file);
			if (bytes)
			{
				ledger = Decode(*bytes); durable = *bytes;
				// 新しい backup や消えた日付がある場合は両方を残し、閲覧だけに限定する。
				if (const auto prior = Read(paths, backup)) Require(Extends(Decode(*prior), ledger), "history-regressed");
			}
			else Require(!Read(paths, backup), "missing-history");
			recovered = ledger.checkpoint.active;
		}
		catch (const std::exception&)
		{
			readOnly = true;
			notice = "history-unavailable";
			// 壊れた原本は残し、検証済み backup は閲覧だけに使う。
			try
			{
				if (const auto bytes = Read(paths, backup))
				{
					ledger = Decode(*bytes); savedTotal = ledger.totalUs;
					recovered = true; notice = "backup-read-only";
				}
			} catch (const std::exception&) {}
		}
		savedRevision = ledger.revision; savedTotal = ledger.totalUs;
		recovered = recovered || ledger.checkpoint.active;
		worker = std::thread([this] { Run(); });
	}
	~Impl()
	{
		{
			std::lock_guard lock(mutex);
			if (active) { Sample(); active = ledger.checkpoint.active = false; ++ledger.revision; }
			stopping = true;
		}
		wake.notify_one();
		if (worker.joinable()) worker.join();
	}
	void Add(std::string date, uint64_t duration)
	{
		if (!duration) return;
		Require(duration <= MaxDuration - ledger.totalUs && (ledger.days.contains(date) || ledger.days.size() < MaxDays), "history-capacity");
		ledger.days[std::move(date)] += duration;
		ledger.totalUs += duration;
		ledger.checkpoint.durationUs += duration;
	}
	void Sample()
	{
		if (!active) return;
		try
		{
			const auto now = clock();
			Require(now.awakeUs >= previous.awakeUs, "clock-failed");
			const auto elapsed = now.awakeUs - previous.awakeUs;
			Require(elapsed <= MaxDuration - ledger.totalUs, "history-capacity");
			// 壁時計の飛躍・sleep・timezone変更は過去の分類を動かさず次の区間へ再anchorする。
			const bool continuous = now.utcUs >= previous.utcUs &&
				(now.utcUs - previous.utcUs > elapsed ? now.utcUs - previous.utcUs - elapsed : elapsed - (now.utcUs - previous.utcUs)) < 2 * Second;
			if (!continuous) Add(LocalDate(previous.utcUs, previous.zone), elapsed);
			else
			{
				// 実経過の総量を維持し、UTC上のローカル日付境界を整数の二分探索で分割する。
				uint64_t cursor = now.utcUs - elapsed;
				while (cursor < now.utcUs)
				{
					const auto date = LocalDate(cursor, previous.zone);
					auto end = std::min(now.utcUs, cursor + Day / 2);
					if (LocalDate(end - 1, previous.zone) != date)
					{
						auto low = cursor + 1, high = end;
						while (low < high)
						{
							const auto middle = low + (high - low) / 2;
							if (LocalDate(middle, previous.zone) == date) low = middle + 1;
							else high = middle;
						}
						end = low;
					}
					Add(date, end - cursor); cursor = end;
				}
			}
			previous = now;
			ledger.checkpoint.sampledUtcUs = now.utcUs;
			if (elapsed) { ++ledger.revision; ++ledger.checkpoint.sequence; }
		}
		catch (const std::exception&)
		{
			active = false; ledger.checkpoint.active = false;
			notice = "tracking-failed";
		}
	}
	void Save(const Ledger& snapshot)
	{
		Require(Valid(snapshot), "invalid-history");
		const auto bytes = glz::write_json(snapshot).value();
		Require(bytes.size() <= MaxBytes, "history-capacity");
		const auto current = Read(paths, file);
		Require(current.value_or("") == durable, "history-changed");
		paths.CheckPath(backup);
		if (current) Require(Extends(Decode(*current), snapshot), "history-regressed");
		if (const auto prior = Read(paths, backup)) Require(Extends(Decode(*prior), snapshot), "history-regressed");
		paths.CheckPath(pending);
		{
			File output{CreateFileW(pending.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
			Require(output.value != INVALID_HANDLE_VALUE, "write-failed");
			OrdinaryFile(output.value);
			Require(SetEndOfFile(output.value), "write-failed");
			DWORD written = 0;
			Require(WriteFile(output.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
				written == bytes.size() && FlushFileBuffers(output.value), "write-failed");
		}
		Require(Read(paths, pending) == bytes, "write-failed");
		// 保存済みtotalを含む完全なsnapshotを置換するため、再読込・retryでも差分を重複適用しない。
		if (current) Require(ReplaceFileW(file.c_str(), pending.c_str(), backup.c_str(), 0, nullptr, nullptr), "replace-failed");
		else Require(MoveFileExW(pending.c_str(), file.c_str(), MOVEFILE_WRITE_THROUGH), "replace-failed");
		Require(Read(paths, file) == bytes, "verify-failed");
		durable = bytes;
	}
	void Run()
	{
		std::unique_lock lock(mutex);
		auto next = steady_clock::now() + CheckpointInterval;
		for (;;)
		{
			wake.wait_until(lock, next, [this] { return stopping || urgent; });
			urgent = false;
			Sample();
			bool saveFailed = false;
			if (!readOnly && ledger.revision != savedRevision)
			{
				const auto snapshot = ledger;
				lock.unlock();
				bool saved = false;
				try { Save(snapshot); saved = true; } catch (const std::exception&) {}
				lock.lock();
				if (saved)
				{
					savedRevision = snapshot.revision; savedTotal = snapshot.totalUs;
					if (notice == "save-failed") notice.clear();
				}
				else { notice = "save-failed"; saveFailed = true; }
			}
			if (stopping)
			{
				if (!readOnly && !saveFailed && ledger.revision != savedRevision) continue;
				return;
			}
			next = steady_clock::now() + CheckpointInterval;
		}
	}
};

SessionHistory::ClockSample SessionHistory::ReadClock()
{
	ClockSample result;
	ULONGLONG awake = 0;
	QueryUnbiasedInterruptTimePrecise(&awake);
	result.awakeUs = awake / 10;
	FILETIME utc{};
	GetSystemTimePreciseAsFileTime(&utc);
	result.utcUs = (uint64_t(utc.dwHighDateTime) << 32 | utc.dwLowDateTime) / 10;
	Require(GetDynamicTimeZoneInformation(&result.zone) != TIME_ZONE_ID_INVALID, "calendar-failed");
	return result;
}
SessionHistory::SessionHistory(const LauncherPaths& paths, Clock clock) : m_impl(std::make_unique<Impl>(paths, std::move(clock))) {}
SessionHistory::~SessionHistory() = default;
void SessionHistory::Begin(uint64_t sessionId)
{
	std::lock_guard lock(m_impl->mutex);
	if (!sessionId || sessionId <= m_impl->seenSession || m_impl->active) return;
	m_impl->seenSession = sessionId;
	try
	{
		m_impl->previous = m_impl->clock();
		m_impl->ledger.checkpoint = {NewId(), sessionId, 1, m_impl->previous.utcUs, m_impl->previous.utcUs, 0, true};
		m_impl->active = true; ++m_impl->ledger.revision;
		m_impl->urgent = true; m_impl->wake.notify_one();
	} catch (const std::exception&) { m_impl->notice = "tracking-failed"; }
}
void SessionHistory::End(uint64_t sessionId)
{
	std::lock_guard lock(m_impl->mutex);
	if (!m_impl->active || sessionId != m_impl->seenSession) return;
	m_impl->Sample();
	m_impl->active = m_impl->ledger.checkpoint.active = false;
	++m_impl->ledger.revision;
	m_impl->urgent = true; m_impl->wake.notify_one();
}
void SessionHistory::ClockChanged()
{
	std::lock_guard lock(m_impl->mutex);
	m_impl->Sample();
}
std::string SessionHistory::NoticeJson() const
{
	std::lock_guard lock(m_impl->mutex);
	return glz::write_json(m_impl->notice).value();
}
std::string SessionHistory::Query(std::string_view payload)
{
	Request request;
	if (payload.empty() || payload.size() > 256 || glz::read_json(request, payload) ||
		(request.mode != "daily" && request.mode != "weekly" && request.mode != "monthly" && request.mode != "yearly")) return R"({"notice":"invalid-request"})";
	std::lock_guard lock(m_impl->mutex);
	try
	{
		m_impl->Sample();
		const auto now = m_impl->clock();
		const auto today = *ParseDate(LocalDate(now.utcUs, now.zone));
		const year_month_day calendar(today);
		if (!request.year) request.year = int(calendar.year());
		if (!request.month) request.month = int(unsigned(calendar.month()));
		Require(request.year >= 1601 && request.year <= 9998 && request.month >= 1 && request.month <= 12, "invalid-request");
		const auto anchor = request.anchor.empty() ? std::optional(today) : ParseDate(request.anchor);
		Require(anchor.has_value() && *anchor >= sys_days(year(1602) / January / 1) && *anchor < sys_days(year(9998) / January / 1), "invalid-request");
		const auto& ledger = m_impl->ledger;
		View view;
		view.notice = m_impl->notice; view.active = m_impl->active; view.recovered = m_impl->recovered; view.readOnly = m_impl->readOnly;
		view.today = Date(today); view.mode = request.mode; view.revision = ledger.revision;
		view.totalSeconds = ledger.totalUs / Second; view.savedSeconds = m_impl->savedTotal / Second;
		view.sessionSeconds = view.active ? ledger.checkpoint.durationUs / Second : 0;
		view.todaySeconds = Sum(ledger.days, today, today + days(1)) / Second;
		const auto week = today - days(weekday(today).iso_encoding() - 1);
		view.weekSeconds = Sum(ledger.days, week, week + days(7)) / Second;
		const auto month = calendar.year() / calendar.month() / 1;
		view.monthSeconds = Sum(ledger.days, sys_days(month), sys_days(month + months(1))) / Second;
		view.yearSeconds = Sum(ledger.days, sys_days(calendar.year() / January / 1), sys_days((calendar.year() + years(1)) / January / 1)) / Second;
		view.firstYear = ledger.days.empty() ? int(calendar.year()) : int(year_month_day(*ParseDate(ledger.days.begin()->first)).year());
		view.lastYear = ledger.days.empty() ? int(calendar.year()) : int(year_month_day(*ParseDate(ledger.days.rbegin()->first)).year());
		view.firstYear = std::min(view.firstYear, int(calendar.year())); view.lastYear = std::max(view.lastYear, int(calendar.year()));
		view.year = request.year; view.month = request.month;
		const auto bucket = [&](sys_days begin, sys_days end) { return Bucket{Date(begin), Date(end - days(1)), Sum(ledger.days, begin, end) / Second}; };
		const year_month_day endCalendar(*anchor);
		if (request.mode == "daily")
			for (int i = 29; i >= 0; --i) view.buckets.push_back(bucket(*anchor - days(i), *anchor - days(i) + days(1)));
		else if (request.mode == "weekly")
		{
			const auto finalWeek = *anchor - days(weekday(*anchor).iso_encoding() - 1);
			for (int i = 11; i >= 0; --i) view.buckets.push_back(bucket(finalWeek - days(i * 7), finalWeek - days(i * 7) + days(7)));
		}
		else if (request.mode == "monthly")
		{
			const auto finalMonth = endCalendar.year() / endCalendar.month() / 1;
			for (int i = 11; i >= 0; --i) view.buckets.push_back(bucket(sys_days(finalMonth - months(i)), sys_days(finalMonth - months(i) + months(1))));
		}
		else
			for (int i = 4; i >= 0; --i) view.buckets.push_back(bucket(sys_days((endCalendar.year() - years(i)) / January / 1), sys_days((endCalendar.year() - years(i) + years(1)) / January / 1)));
		const sys_days first(year(request.year) / std::chrono::month(request.month) / 1);
		const sys_days monthEnd(year_month_day(first) + months(1));
		for (auto date = first; date < monthEnd; date += days(1)) view.dates.push_back(bucket(date, date + days(1)));
		return glz::write_json(view).value();
	}
	catch (const std::exception&) { return R"({"notice":"invalid-request"})"; }
}
