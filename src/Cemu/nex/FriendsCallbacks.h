#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <tuple>
#include <vector>

// Guest の出力先は FPD thread でだけ参照し、Title Stop の取消を越えない。
class FriendsCallbacks
{
	struct State
	{
		std::recursive_mutex mutex;
		uint64_t generation = 0;
		bool active = false;
		std::vector<std::function<void()>> pending;
	};
	std::shared_ptr<State> m_state = std::make_shared<State>();
	template<typename T>
	static auto Copy(T value)
	{
		return value;
	}
	template<typename T>
	static auto Copy(std::span<T> value)
	{
		return std::vector<T>(value.begin(), value.end());
	}

  public:
	void Open()
	{
		std::lock_guard lock(m_state->mutex);
		++m_state->generation;
		m_state->pending.clear();
		m_state->active = true;
	}
	void Close()
	{
		std::lock_guard lock(m_state->mutex);
		++m_state->generation;
		m_state->active = false;
		m_state->pending.clear();
	}
	bool IsOpen() const
	{
		std::lock_guard lock(m_state->mutex);
		return m_state->active;
	}
	template<typename F> void Run(F callback)
	{
		std::lock_guard lock(m_state->mutex);
		if (m_state->active) callback();
	}
	void Drain()
	{
		std::lock_guard lock(m_state->mutex);
		if (!m_state->active)
			return;
		auto pending = std::move(m_state->pending);
		m_state->pending.clear();
		for (auto& callback : pending)
			callback();
	}
	template<typename F>
	auto Wrap(F callback)
	{
		std::lock_guard lock(m_state->mutex);
		const auto generation = m_state->generation;
		return [weak = std::weak_ptr<State>(m_state), generation, callback](auto... args) {
			if (auto state = weak.lock())
			{
				std::lock_guard lock(state->mutex);
				if (!state->active || state->generation != generation)
					return;
				auto values = std::make_tuple(Copy(args)...);
				state->pending.emplace_back([callback, values = std::move(values)]() mutable { std::apply(callback, values); });
			}
		};
	}
};
