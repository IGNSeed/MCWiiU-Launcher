#include "FriendsSession.h"
#include "nex.h"
#include "nexFriends.h"
#include "Cafe/Account/Account.h"
#include "Cemu/napi/napi.h"
#include "config/ActiveSettings.h"
#include "Common/socket.h"
#include "util/helpers/helpers.h"
#include <condition_variable>

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
				next.requests.push_back({pids[i], PublicText(info.nnid, 31), PublicText(info.mii.miiNickname, 127), PublicText(request.message.commentStr, 1023), incoming});
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
			if (!session && refresh && Clock::now() >= nextRefresh)
			{
				refresh = false;
				nextRefresh = Clock::now() + std::chrono::seconds(5);
				retry = {};
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
					if (!session->isConnected() || (inFlight && Clock::now() >= requestDeadline))
					{
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
						if (session->isOnline() && !inFlight && (refresh || Clock::now() >= autoRefresh) && Clock::now() >= nextRefresh)
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
					wake.wait(lock, [&] { return stopped || generation != current; });
				else if (!session)
					wake.wait_until(lock, retry, [&] { return stopped || generation != current || refresh; });
				else
					wake.wait_for(lock, std::chrono::milliseconds(10));
			}
		}
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
		if (service != NetworkService::Offline && account.IsValidOnlineAccount() && ActiveSettings::HasRequiredOnlineFiles() && NAPI::NAPI_MakeAuthInfoFromCurrentAccount(config->auth))
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
		state.worker = std::thread([&state] { try { state.Run(); } catch (...) { std::lock_guard lock(state.mutex); state.session.reset(); state.Publish("network-error"); } });
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
