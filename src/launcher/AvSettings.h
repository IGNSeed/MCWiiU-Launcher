#pragma once

#include <memory>
#include <string>
#include <string_view>

// Core の Ready 状態で、既存 Cemu 設定の編集とデバイス列挙を行う。
class AvSettings final
{
public:
	AvSettings();
	~AvSettings();
	std::string Snapshot();
	std::string Command(std::string_view payload, bool running = false);
	void PrepareLaunch();
	void EndTitle();
	bool IsHealthy() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
