#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// SDK と切り離した固定文言だけを扱い、ゲーム・個人情報は受け取らない。
class DiscordPresenceState final
{
public:
	static constexpr uint64_t ApplicationId = 1573624989045887027ULL;
	static constexpr std::string_view LargeImage = "mcwiiu_launcher";
	static constexpr std::string_view LargeText = "MCWiiU Launcher";

	struct Activity
	{
		std::string_view details, state;
		uint64_t start;
		bool operator==(const Activity&) const = default;
	};

	explicit DiscordPresenceState(uint64_t processStart) : m_start(processStart) {}

	bool SetPage(std::string_view page)
	{
		for (std::size_t index = 0; index < s_pages.size(); ++index)
		{
			if (s_pages[index] != page) continue;
			m_page = index;
			return true;
		}
		return false;
	}
	void SetPlaying(bool playing) { m_playing = playing; }
	Activity Current() const
	{
		return {m_playing ? "Playing Minecraft: Wii U Edition" : s_details[m_page],
			m_playing ? "In Game" : "In the Launcher", m_start};
	}

private:
	static constexpr std::array<std::string_view, 8> s_pages{
		"home", "friends", "world", "texture", "graphic-packs", "session-history", "settings", "setup"};
	static constexpr std::array<std::string_view, 8> s_details{
		"Browsing Home", "Browsing Friends", "Browsing World", "Browsing Texture", "Browsing Graphic Packs",
		"Viewing Session History", "Browsing Settings", "Setting Up MCWiiU"};
	const uint64_t m_start;
	std::size_t m_page = 0;
	bool m_playing = false;
};
