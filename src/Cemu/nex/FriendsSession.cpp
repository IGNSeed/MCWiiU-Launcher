#include "FriendsSession.h"
#include "nex.h"
#include "nexFriends.h"
#include "Cafe/Account/Account.h"
#include "Cemu/napi/napi.h"
#include "config/ActiveSettings.h"
#include "Common/socket.h"
#include "util/helpers/helpers.h"
#include <condition_variable>
#include <optional>

namespace
{
	using Clock = std::chrono::steady_clock;
	std::string PublicText(std::string_view text, size_t limit)
	{
		if (text.size() > limit || text.find('\0') != std::string_view::npos)
			return {};
		// UTF-8 の非最短形、surrogate、範囲外の文字を表示境界で拒否する。
		for (size_t i = 0; i < text.size();)
		{
			const auto first = static_cast<unsigned char>(text[i++]);
			if (first < 0x80)
			{
				if (first < 0x20 && first != '\n' && first != '\t')
					return {};
				continue;
			}
			unsigned count = first >= 0xf0 ? 3 : first >= 0xe0 ? 2
											 : first >= 0xc2   ? 1
															   : 0;
			if (!count || first > 0xf4 || i + count > text.size())
				return {};
			uint32_t code = first & ((1u << (6 - count)) - 1);
			for (unsigned n = 0; n < count; ++n)
			{
				const auto c = static_cast<unsigned char>(text[i++]);
				if ((c & 0xc0) != 0x80)
					return {};
				code = (code << 6) | (c & 0x3f);
			}
			if (code < (count == 1 ? 0x80u : count == 2 ? 0x800u
														: 0x10000u) ||
				code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
				return {};
		}
		return std::string(text);
	}
	template<size_t N>
	std::string PublicText(const char (&text)[N], size_t limit)
	{
		const auto end = std::find(std::begin(text), std::end(text), '\0');
		if (end == std::end(text))
			return {};
		return PublicText(std::string_view(text, end - text), limit);
	}

	std::string FoldId(std::string value)
	{
		for (auto& c : value)
			if (c >= 'A' && c <= 'Z')
				c += 'a' - 'A';
		return value;
	}
	bool Expired(uint64_t timestamp)
	{
		const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
		const auto day = std::chrono::floor<std::chrono::days>(now);
		const std::chrono::year_month_day date(day);
		const std::chrono::hh_mm_ss time(now - day);
		const uint64_t encoded = (uint64_t(int(date.year())) << 26) | (uint64_t(unsigned(date.month())) << 22) | (uint64_t(unsigned(date.day())) << 17) |
								 (uint64_t(time.hours().count()) << 12) | (uint64_t(time.minutes().count()) << 6) | uint64_t(time.seconds().count());
		return timestamp && timestamp <= encoded;
	}
} // namespace

struct FriendsSession::Impl
{
	struct Configuration
	{
		uint32_t activeId = 0, pid = 0;
		uint8_t country = 0;
		std::string service = "Offline", state = "offline";
		NAPI::AuthInfo auth{};
		std::array<uint8_t, 96> mii{};
		std::array<uint16_t, 11> name{};
	};
	std::recursive_mutex mutex;
	mutable std::mutex viewMutex;
	std::condition_variable_any wake;
	std::thread worker;
	std::atomic_bool stopped{false};
	std::atomic_uint64_t generation{0};
	std::shared_ptr<Configuration> configuration;
	std::unique_ptr<NexFriends> session;
	uint64_t sessionGeneration = 0, titleId = 0, titleRevision = 0, connectionSerial = 0;
	uint16_t titleVersion = 0;
	bool keepAlive = false, refresh = false, inFlight = false, reconnect = false;
	Clock::time_point nextRefresh{};
	void (*notification)(int, uint32_t) = nullptr;
	View view;

	std::optional<Command> command;
	uint64_t lastCommandId = 0;
	std::string commandPhase;
	Clock::time_point commandDeadline{}, syncAt{};
	bool unknownOutcome = false;
	struct Resolution
	{
		std::atomic_bool cancelled{false}, done{false};
		NAPI::ACTConvertNnidToPrincipalIdResult result{};
	};
	std::shared_ptr<Resolution> resolution;
	std::thread resolver;
	bool Matches(const Command& target) const
	{
		return !stopped && command && command->id == target.id && generation == target.generation && sessionGeneration == target.generation &&
			   view.connectionId == target.connectionId && configuration && configuration->service == "Pretendo";
	}
	void OperationStatus(std::string phase, std::string notice = {})
	{
		commandPhase = phase;
		std::lock_guard lock(viewMutex);
		view.operation = {command ? command->id : 0, phase, std::move(notice), phase == "queued" || phase == "resolving" || phase == "checking" || phase == "sent" || phase == "syncing"};
		++view.revision;
	}
	void CancelCommand()
	{
		if (resolution)
			resolution->cancelled = true;
		command.reset();
		commandPhase.clear();
		std::lock_guard lock(viewMutex);
		view.operation = {};
		++view.revision;
	}
	std::string ValidateTarget(const Command& target)
	{
		if (!configuration || target.pid == configuration->pid)
			return "self-request";
		nexFriend existing{};
		const bool isFriend = session->getFriendByPID(existing, target.pid);
		if (target.operation == Operation::Remove)
			return isFriend ? "" : "friend-not-found";
		nexFriendRequest request{};
		bool incoming = false;
		if (target.operation == Operation::Send)
		{
			if (isFriend)
				return "already-friends";
			return session->getFriendRequestByPID(request, &incoming, target.pid) ? "already-requested" : "";
		}
		if (isFriend)
			return "already-friends";
		if (!session->getFriendRequestByMessageId(request, &incoming, target.messageId) || request.principalInfo.principalId != target.pid ||
			incoming != (target.operation != Operation::Cancel))
			return "request-not-found";
		if (!request.message.expireTimestamp)
			return "unsupported-request";
		return target.operation == Operation::Accept && Expired(request.message.expireTimestamp) ? "request-expired" : "";
	}
	bool Reflected(const Command& target)
	{
		nexFriend friendData{};
		nexFriendRequest request{};
		bool incoming = false;
		const bool isFriend = session->getFriendByPID(friendData, target.pid);
		if (target.operation == Operation::Send)
			return isFriend || (session->getFriendRequestByPID(request, &incoming, target.pid) && !incoming);
		if (target.operation == Operation::Remove)
			return !isFriend;
		const bool exists = session->getFriendRequestByMessageId(request, &incoming, target.messageId);
		if (target.operation == Operation::Accept)
			return isFriend && !exists;
		return !exists && !isFriend;
	}
	void Dispatch(Command target)
	{
		if (!Matches(target))
			return;
		if (auto error = ValidateTarget(target); !error.empty())
		{
			OperationStatus("failed", error);
			return;
		}
		OperationStatus("sent");
		commandDeadline = Clock::now() + std::chrono::seconds(15);
		NexFriends::ManagementOperation operation;
		switch (target.operation)
		{
		case Operation::Send:
			operation = NexFriends::ManagementOperation::Send;
			break;
		case Operation::Remove:
			operation = NexFriends::ManagementOperation::Remove;
			break;
		case Operation::Accept:
			operation = NexFriends::ManagementOperation::Accept;
			break;
		case Operation::Reject:
			operation = NexFriends::ManagementOperation::Reject;
			break;
		case Operation::Cancel:
			operation = NexFriends::ManagementOperation::Cancel;
			break;
		default:
			OperationStatus("failed", "invalid-command");
			return;
		}
		session->manageFriend(operation, target.pid, target.messageId, [this, target](int result, uint32_t detail) {
			if (!Matches(target) || commandPhase != "sent")
				return;
			if (result != NexFriends::ERR_NONE && detail != nexService::ERR_TIMEOUT && detail != nexService::ERR_NO_CONNECTION)
			{
				cemuLog_log(LogType::Force, "Friends management RPC rejected: 0x{:08x}", detail);
				OperationStatus("failed", "server-error");
				refresh = true;
				return;
			}
			unknownOutcome = result != NexFriends::ERR_NONE;
			OperationStatus("syncing", unknownOutcome ? "outcome-unknown" : "");
			commandDeadline = Clock::now() + std::chrono::seconds(20);
			syncAt = {};
		});
	}
	void TickCommand()
	{
		// ACT解決中も既存WorkerのNEX pumpを継続する。補助Threadは1件だけ所有して必ずJoinする。
		if (resolution && resolution->done)
		{
			resolver.join();
			auto result = std::move(resolution);
			if (command && commandPhase == "resolving" && Matches(*command))
			{
				if (!result->result.isValid())
					OperationStatus("failed", "lookup-failed");
				else if (!result->result.isFound || !result->result.principalId)
					OperationStatus("failed", "user-not-found");
				else
				{
					command->pid = result->result.principalId;
					OperationStatus("queued");
				}
			}
		}
		if (!command || !Matches(*command) || !session || !session->isOnline())
			return;
		if (commandPhase == "queued")
		{
			if (command->operation == Operation::Send && !command->pid)
			{
				if (resolution)
					return;
				OperationStatus("resolving");
				resolution = std::make_shared<Resolution>();
				auto result = resolution;
				auto auth = configuration->auth;
				const auto target = *command;
				resolver = std::thread([this, result, auth = std::move(auth), target]() mutable {
					SetThreadName("FriendsPnid");
					auth.cancelled = [this, result, expected = target.generation, deadline = Clock::now() + std::chrono::seconds(15)] {
						return stopped || result->cancelled || generation != expected || Clock::now() >= deadline;
					};
					try
					{
						result->result = NAPI::ACT_ACTConvertNnidToPrincipalId(auth, target.accountId);
					} catch (...)
					{
						result->result.apiError = NAPI_RESULT::FAILED;
					}
					if (auth.cancelled())
						result->result.apiError = NAPI_RESULT::FAILED;
					result->done = true;
					wake.notify_all();
				});
				return;
			}
			if (inFlight)
				return;
			OperationStatus("checking");
			inFlight = true;
			const auto target = *command;
			commandDeadline = Clock::now() + std::chrono::seconds(12);
			session->requestGetAllInformation([this, target](uint32 result) {
				if (!Matches(target) || commandPhase != "checking")
					return;
				inFlight = false;
				if (result != NexFriends::ERR_NONE)
					OperationStatus("failed", "refresh-failed");
				else
				{
					Capture();
					Dispatch(target);
				}
			});
		}
		else if (commandPhase == "sent" && Clock::now() >= commandDeadline)
		{
			// 応答の喪失時に変更RPCを再送しない。まずサーバー状態を確認する。
			unknownOutcome = true;
			OperationStatus("syncing", "outcome-unknown");
			commandDeadline = Clock::now() + std::chrono::seconds(20);
			syncAt = {};
		}
		else if (commandPhase == "checking" && Clock::now() >= commandDeadline)
		{
			inFlight = false;
			OperationStatus("failed", "refresh-failed");
		}
		else if (commandPhase == "syncing")
		{
			if (Clock::now() >= commandDeadline)
			{
				inFlight = false;
				OperationStatus("uncertain", unknownOutcome ? "outcome-unknown" : "sync-pending");
				return;
			}
			if (inFlight || Clock::now() < syncAt)
				return;
			inFlight = true;
			syncAt = Clock::now() + std::chrono::seconds(2);
			const auto target = *command;
			session->requestGetAllInformation([this, target](uint32 result) {
				if (!Matches(target) || commandPhase != "syncing")
					return;
				inFlight = false;
				if (result == NexFriends::ERR_NONE)
				{
					Capture();
					if (Reflected(target))
						OperationStatus("succeeded", unknownOutcome ? "state-confirmed" : "saved");
				}
			});
		}
	}

	static void Notify(NexFriends::NOTIFICATION_TYPE type, uint32_t pid)
	{
		auto& state = *FriendsSession::Instance().m_impl;
		if (state.notification)
			state.notification(static_cast<int>(type), pid);
	}

	void Publish(std::string status)
	{
		std::lock_guard lock(viewMutex);
		view.status = std::move(status);
		view.refreshing = inFlight;
		++view.revision;
		if (view.status != "connected")
		{
			view.friends.clear();
			view.requests.clear();
		}
	}
	void Capture()
	{
		View next;
		{
			std::lock_guard lock(viewMutex);
			next = view;
		}
		next.friends.clear();
		next.requests.clear();
		next.status = "connected";
		next.refreshing = inFlight;
		next.preferencesUnset = session->preferencesUnset();
		std::array<uint32_t, 1000> pids{};
		uint32_t count = 0;
		session->getFriendPIDs(pids.data(), &count, 0, int(pids.size()), false);
		for (uint32_t i = 0; i < count; ++i)
		{
			nexFriend friendData{};
			if (!pids[i] || !session->getFriendByPID(friendData, pids[i]))
				continue;
			const auto& info = friendData.nnaInfo.principalInfo;
			next.friends.push_back({pids[i], PublicText(info.nnid, 31), PublicText(info.mii.miiNickname, 127), friendData.presence.isOnline != 0, friendData.presence.gameKey.titleId, friendData.lastOnlineTimestamp});
		}
		for (bool incoming : {true, false})
		{
			session->getFriendRequestPIDs(pids.data(), &count, 0, int(pids.size()), incoming, !incoming);
			for (uint32_t i = 0; i < count; ++i)
			{
				nexFriendRequest request{};
				bool direction = false;
				if (!pids[i] || !session->getFriendRequestByPID(request, &direction, pids[i]) || direction != incoming)
					continue;
				const auto& info = request.principalInfo;
				next.requests.push_back({pids[i], PublicText(info.nnid, 31), PublicText(info.mii.miiNickname, 127), PublicText(request.message.commentStr, 1023), incoming, request.message.messageId, request.message.messageId != 0 && request.message.expireTimestamp != 0, Expired(request.message.expireTimestamp)});
			}
		}
		std::lock_guard lock(viewMutex);
		if (next.friends != view.friends || next.requests != view.requests || next.status != view.status || next.refreshing != view.refreshing || next.preferencesUnset != view.preferencesUnset)
		{
			next.revision = view.revision + 1;
			view = std::move(next);
		}
	}
	std::unique_ptr<NexFriends> Connect(const Configuration& config, uint64_t expected)
	{
		auto cancelled = [this, expected, deadline = Clock::now() + std::chrono::seconds(45)] {
			return stopped || generation != expected || Clock::now() >= deadline;
		};
		auto auth = config.auth;
		auth.cancelled = cancelled;
		const auto token = NAPI::ACT_GetNexToken_WithCache(auth, 0x0005001010001C00, 0, 0x3200);
		if (cancelled())
			return {};
		if (!token.isValid())
		{
			std::lock_guard lock(mutex);
			if (generation == expected)
				Publish(token.apiError == NAPI_RESULT::SERVICE_ERROR ? "authentication-failed" : "network-error");
			return {};
		}
		addrinfo hints{}, *addresses = nullptr;
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		if (getaddrinfo(token.nexToken.host, nullptr, &hints, &addresses) != 0)
		{
			std::lock_guard lock(mutex);
			if (generation == expected)
				Publish("server-unavailable");
			return {};
		}
		const auto ip = reinterpret_cast<sockaddr_in*>(addresses->ai_addr)->sin_addr.s_addr;
		freeaddrinfo(addresses);
		if (cancelled())
			return {};
		nexPresenceV2 presence{};
		{
			std::lock_guard lock(mutex);
			presence.isOnline = 1;
			presence.gameKey.titleId = titleId;
			presence.gameKey.ukn = titleVersion;
		}
		auto mii = config.mii;
		auto candidate = std::make_unique<NexFriends>(ip, token.nexToken.port, "ridfebb9", config.pid, token.nexToken.nexPassword, token.nexToken.token, config.auth.accountId.c_str(), mii.data(), config.name.data(), config.country, presence);
		if (!candidate->connect(cancelled) || cancelled())
		{
			std::lock_guard lock(mutex);
			if (generation == expected)
				Publish("server-unavailable");
			return {};
		}
		candidate->setNotificationHandler(Notify);
		return candidate;
	}
	void Run()
	{
		SetThreadName("FriendsSession");
		uint64_t current = 0, appliedTitle = 0;
		unsigned failures = 0;
		Clock::time_point retry{}, requestDeadline{}, captureAt{}, autoRefresh{};
		while (!stopped)
		{
			std::shared_ptr<Configuration> config;
			uint64_t expected;
			{
				std::lock_guard lock(mutex);
				expected = generation;
				config = configuration;
				if (current != expected)
				{
					CancelCommand();
					session.reset();
					current = expected;
					failures = 0;
					retry = {};
					inFlight = false;
					reconnect = false;
					refresh = false;
					nextRefresh = {};
				}
				if (reconnect)
				{
					session.reset();
					reconnect = false;
					inFlight = false;
					retry = Clock::now() + std::chrono::seconds(std::min(60u, 8u << std::min(failures++, 3u)));
				}
			}
			{
				std::lock_guard lock(mutex);
				if (!session && refresh && Clock::now() >= nextRefresh)
				{
					refresh = false;
					nextRefresh = Clock::now() + std::chrono::seconds(5);
					retry = {};
				}
			}
			if (config && config->state == "ready" && !session && Clock::now() >= retry)
			{
				{
					std::lock_guard lock(mutex);
					if (expected == generation)
						Publish(failures ? "reconnecting" : "connecting");
				}
				auto candidate = Connect(*config, expected);
				std::lock_guard lock(mutex);
				if (!stopped && expected == generation && candidate)
				{
					session = std::move(candidate);
					sessionGeneration = expected;
					{
						std::lock_guard viewLock(viewMutex);
						view.connectionId = ++connectionSerial;
					}
					inFlight = true;
					requestDeadline = Clock::now() + std::chrono::seconds(12);
					appliedTitle = 0;
				}
				else
				{
					candidate.reset();
					retry = Clock::now() + std::chrono::seconds(std::min(60u, 8u << std::min(failures++, 3u)));
				}
			}
			{
				std::unique_lock lock(mutex);
				if (resolution && resolution->done)
					TickCommand();
				if (session && current == generation)
				{
					if (appliedTitle != titleRevision)
					{
						nexPresenceV2 presence{};
						presence.isOnline = 1;
						presence.gameKey.titleId = titleId;
						presence.gameKey.ukn = titleVersion;
						session->updateMyPresence(presence);
						appliedTitle = titleRevision;
					}
					session->update();
					TickCommand();
					if (!session->isConnected() || (inFlight && commandPhase != "checking" && commandPhase != "syncing" && Clock::now() >= requestDeadline))
					{
						if (command && (commandPhase == "sent" || commandPhase == "syncing"))
							OperationStatus("uncertain", "connection-lost");
						else if (command && (commandPhase == "queued" || commandPhase == "checking" || commandPhase == "resolving"))
							OperationStatus("failed", "connection-lost");
						Publish("network-error");
						reconnect = true;
					}
					else
					{
						if (session->isOnline() && inFlight && view.status != "connected")
						{
							inFlight = false;
							autoRefresh = Clock::now() + std::chrono::minutes(5);
						}
						if (session->isOnline() && !inFlight && (!command || (commandPhase != "checking" && commandPhase != "syncing")) && (refresh || Clock::now() >= autoRefresh) && Clock::now() >= nextRefresh)
						{
							refresh = false;
							inFlight = true;
							requestDeadline = Clock::now() + std::chrono::seconds(12);
							nextRefresh = Clock::now() + std::chrono::seconds(5);
							autoRefresh = Clock::now() + std::chrono::minutes(5);
							session->requestGetAllInformation([this, expected](uint32 result) { if (expected != generation) return; inFlight = false; if (result != NexFriends::ERR_NONE) { Publish("network-error"); reconnect = true; } });
						}
						if (!reconnect && session->isOnline() && Clock::now() >= captureAt)
						{
							Capture();
							captureAt = Clock::now() + std::chrono::seconds(1);
						}
					}
				}
				if (!session && (!config || config->state != "ready"))
					wake.wait(lock, [&] { return stopped || generation != current || (resolution && resolution->done); });
				else if (!session)
					wake.wait_until(lock, retry, [&] { return stopped || generation != current || refresh; });
				else
					wake.wait_for(lock, std::chrono::milliseconds(10));
			}
		}
		CancelCommand();
		if (resolver.joinable())
			resolver.join();
		resolution.reset();
		std::lock_guard lock(mutex);
		session.reset();
		inFlight = false;
		Publish("offline");
	}
};

FriendsSession::FriendsSession() : m_impl(std::make_unique<Impl>()) {}
FriendsSession::~FriendsSession()
{
	Shutdown();
}
FriendsSession& FriendsSession::Instance()
{
	static FriendsSession instance;
	return instance;
}
void FriendsSession::ConfigureCurrent(bool launcher, std::string requestedService)
{
	auto& state = *m_impl;
	auto config = std::make_shared<Impl::Configuration>();
	const auto& account = Account::GetCurrentAccount();
	config->activeId = account.GetPersistentId();
	config->pid = account.GetPrincipalId();
	const auto service = ActiveSettings::GetNetworkService();
	config->service = service == NetworkService::Pretendo ? "Pretendo" : service == NetworkService::Custom ? "Custom"
																										   : "Offline";
	if (!requestedService.empty())
		config->service = std::move(requestedService);
	if (config->service != "Offline")
	{
		config->state = "account-not-ready";
		if (service != NetworkService::Offline && account.IsValidOnlineAccount() && ActiveSettings::HasRequiredOnlineFiles() && NAPI::NAPI_MakeAuthInfoFromCurrentAccount(config->auth) && !config->auth.accountId.empty() && PublicText(config->auth.accountId, 31) == config->auth.accountId && std::none_of(config->auth.accountId.begin(), config->auth.accountId.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; }))
		{
			config->auth.serviceOverwrite = service;
			config->state = "ready";
			config->mii = account.GetMiiData();
			config->country = static_cast<uint8_t>(account.GetCountry());
			const auto name = account.GetMiiName();
			for (size_t i = 0; i < std::min(name.size(), config->name.size() - 1); ++i)
				config->name[i] = static_cast<uint16_t>(name[i]);
		}
	}
	std::lock_guard lock(state.mutex);
	if (state.stopped)
		return;
	state.keepAlive |= launcher;
	if (state.configuration && state.configuration->activeId == config->activeId && state.configuration->pid == config->pid && state.configuration->service == config->service && state.configuration->state == config->state && state.configuration->auth.passwordHash == config->auth.passwordHash && state.configuration->auth.accountId == config->auth.accountId && state.configuration->mii == config->mii && state.configuration->name == config->name)
		return;
	state.CancelCommand();
	state.configuration = std::move(config);
	++state.generation;
	{
		std::lock_guard viewLock(state.viewMutex);
		state.view = {};
		state.view.generation = state.generation;
		state.view.activeId = state.configuration->activeId;
		state.view.service = state.configuration->service;
	}
	state.Publish(state.configuration->state == "ready" ? "connecting" : state.configuration->state);
	if (!state.worker.joinable())
		state.worker = std::thread([&state] { try { state.Run(); } catch (...) { std::lock_guard lock(state.mutex); state.CancelCommand(); if (state.resolver.joinable()) state.resolver.join(); state.resolution.reset(); state.session.reset(); state.Publish("network-error"); } });
	state.wake.notify_all();
}
void FriendsSession::SetTitle(uint64_t titleId, uint16_t version)
{
	std::lock_guard lock(m_impl->mutex);
	m_impl->titleId = (titleId >> 32) == 0x00050000 ? titleId : 0;
	m_impl->titleVersion = version;
	++m_impl->titleRevision;
	m_impl->wake.notify_all();
}
void FriendsSession::DetachTitle()
{
	SetTitle(0, 0);
	std::lock_guard lock(m_impl->mutex);
	if (!m_impl->keepAlive)
	{
		m_impl->configuration.reset();
		++m_impl->generation;
		m_impl->Publish("offline");
	}
}
void FriendsSession::Refresh()
{
	std::lock_guard lock(m_impl->mutex);
	if (!m_impl->inFlight && Clock::now() >= m_impl->nextRefresh)
	{
		m_impl->refresh = true;
		m_impl->wake.notify_all();
	}
}
bool FriendsSession::ValidatePnid(std::string& pnid)
{
	// Pretendoの登録規則と同じASCII文字種・長さを使い、前後の通常空白だけを除く。
	const auto first = pnid.find_first_not_of(' ');
	if (first == std::string::npos)
		return false;
	pnid = pnid.substr(first, pnid.find_last_not_of(' ') - first + 1);
	if (pnid.size() < 6 || pnid.size() > 16)
		return false;
	const auto alnum = [](unsigned char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
	};
	if (!alnum(pnid.front()) || !alnum(pnid.back()))
		return false;
	bool punctuation = false;
	for (unsigned char c : pnid)
	{
		const bool current = !alnum(c);
		if (current && (punctuation || (c != '_' && c != '-' && c != '.')))
			return false;
		punctuation = current;
	}
	return true;
}
std::string FriendsSession::Submit(Command target)
{
	auto& state = *m_impl;
	std::lock_guard lock(state.mutex);
	if (!target.id || target.id > 9007199254740991ULL || target.id <= state.lastCommandId)
		return "duplicate-request";
	state.lastCommandId = target.id;
	if (state.stopped || !state.configuration || state.configuration->service != "Pretendo")
		return "offline";
	if (target.generation != state.generation || target.connectionId != state.view.connectionId || !target.connectionId)
		return "stale-session";
	if (!state.session || !state.session->isOnline() || state.view.status != "connected")
		return "not-connected";
	if (state.resolution || state.commandPhase == "queued" || state.commandPhase == "resolving" || state.commandPhase == "checking" || state.commandPhase == "sent" || state.commandPhase == "syncing")
		return "operation-busy";
	if (target.operation == Operation::Send)
	{
		if (target.pid || target.messageId || !ValidatePnid(target.accountId))
			return "invalid-pnid";
		const auto folded = FoldId(target.accountId);
		if (folded == FoldId(state.configuration->auth.accountId))
			return "self-request";
		for (const auto& friendData : state.view.friends)
			if (folded == FoldId(friendData.accountId))
				return "already-friends";
		for (const auto& request : state.view.requests)
			if (folded == FoldId(request.accountId))
				return "already-requested";
	}
	else
	{
		if (!target.pid || !target.accountId.empty() || (target.operation == Operation::Remove ? target.messageId != 0 : target.messageId == 0))
			return "invalid-command";
		switch (target.operation)
		{
		case Operation::Remove:
		case Operation::Accept:
		case Operation::Reject:
		case Operation::Cancel:
			break;
		default:
			return "invalid-command";
		}
		if (auto error = state.ValidateTarget(target); !error.empty())
			return error;
	}
	state.command = std::move(target);
	state.unknownOutcome = false;
	state.OperationStatus("queued");
	state.wake.notify_all();
	return "queued";
}
FriendsSession::View FriendsSession::Snapshot() const
{
	std::lock_guard lock(m_impl->viewMutex);
	return m_impl->view;
}
FriendsSession::Lease FriendsSession::Acquire()
{
	m_impl->mutex.lock();
	return Lease(m_impl->mutex, m_impl->sessionGeneration == m_impl->generation ? m_impl->session.get() : nullptr);
}
void FriendsSession::SetNotificationHandler(void (*handler)(int, uint32_t))
{
	std::lock_guard lock(m_impl->mutex);
	m_impl->notification = handler;
}
void FriendsSession::Shutdown()
{
	if (m_impl->stopped.exchange(true))
		return;
	++m_impl->generation;
	m_impl->wake.notify_all();
	if (m_impl->worker.joinable())
		m_impl->worker.join();
}
