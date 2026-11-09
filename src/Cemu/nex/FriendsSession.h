#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class NexFriends;

// 通信は Core の Worker が所有し、FPD は scope 内だけセッションを利用する。
class FriendsSession
{
  public:
	struct Friend
	{
		uint32_t pid = 0;
		std::string accountId, displayName;
		bool online = false;
		uint64_t titleId = 0, lastOnline = 0;
		bool operator==(const Friend&) const = default;
	};
	struct Request
	{
		uint32_t pid = 0;
		std::string accountId, displayName, message;
		bool incoming = false;
		uint64_t messageId = 0;
		bool manageable = false;
		bool expired = false;
		bool operator==(const Request&) const = default;
	};
	enum class Operation
	{
		Send,
		Remove,
		Accept,
		Reject,
		Cancel
	};
	struct Command
	{
		uint64_t id = 0, generation = 0, connectionId = 0, messageId = 0;
		uint32_t pid = 0;
		Operation operation = Operation::Send;
		std::string accountId;
	};
	struct OperationState
	{
		uint64_t id = 0;
		std::string state, notice;
		bool busy = false;
	};
	struct View
	{
		uint64_t generation = 0, revision = 0, connectionId = 0;
		uint32_t activeId = 0;
		std::string service = "Offline", status = "offline";
		bool refreshing = false, preferencesUnset = false;
		OperationState operation;
		std::vector<Friend> friends;
		std::vector<Request> requests;
	};
	class Lease
	{
		std::unique_lock<std::recursive_mutex> m_lock;
		NexFriends* m_session;
		friend class FriendsSession;
		Lease(std::recursive_mutex& mutex, NexFriends* session) : m_lock(mutex, std::adopt_lock), m_session(session) {}

	  public:
		explicit operator bool() const
		{
			return m_session != nullptr;
		}
		NexFriends* operator->() const
		{
			return m_session;
		}
	};
	static FriendsSession& Instance();
	void ConfigureCurrent(bool launcher, std::string requestedService = {});
	void SetTitle(uint64_t titleId, uint16_t version);
	void DetachTitle();
	void Refresh();
	std::string Submit(Command command);
	static bool ValidatePnid(std::string& pnid);
	View Snapshot() const;
	Lease Acquire();
	void SetNotificationHandler(void (*handler)(int, uint32_t));
	void Shutdown();

  private:
	FriendsSession();
	~FriendsSession();
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
