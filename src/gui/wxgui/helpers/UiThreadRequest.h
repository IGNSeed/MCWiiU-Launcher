#pragma once

#include <atomic>
#include <future>

class UiThreadRequest
{
  public:
	bool IsPending() const
	{
		return m_pending.load();
	}

	void Complete(bool executed)
	{
		// 終了による取消と GUI 側の完了が重なっても、待機を一度だけ解除する。
		bool pending = true;
		if (m_pending.compare_exchange_strong(pending, false))
			m_completion.set_value(executed);
	}

	bool Wait()
	{
		return m_result.get();
	}

  private:
	std::promise<bool> m_completion;
	std::future<bool> m_result = m_completion.get_future();
	std::atomic_bool m_pending{true};
};
