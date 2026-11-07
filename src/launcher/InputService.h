#pragma once

#include <memory>
#include <string>
#include <string_view>

// 呼出し元が Core の寿命と Ready 状態を直列化する。
class InputService final
{
public:
	InputService();
	~InputService();
	std::string Snapshot(bool editable = true);
	std::string Command(std::string_view payload);
	std::string Poll();
	void CancelCapture();
	bool IsHealthy() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
