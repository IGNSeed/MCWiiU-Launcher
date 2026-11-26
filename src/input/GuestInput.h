#pragma once

#include "GuestInputAbi.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>

namespace guestinput
{
	struct Viewport
	{
		int32_t x{}, y{}, width{}, height{};
		uint32_t display{}; // 0=TV, 1=DRC
	};

	struct Route
	{
		bool mouse{};
		bool keyboard{};
	};

	class Provider final
	{
	  public:
		using Clock = uint64_t (*)();
		explicit Provider(Clock clock = nullptr);
		void BeginSession();
		void EndSession();
		bool Available() const;
		void Focus(bool focused);
		void Pointer(uint32_t type, int32_t x, int32_t y, const Viewport& viewport,
					 uint32_t button = 0, bool down = false, int32_t wheelX = 0, int32_t wheelY = 0);
		void Key(uint32_t key, bool down, bool repeat = false);
		void Character(uint32_t utf16);
		Route LegacyRoute();
		bool LegacyKeyAllowed(uint32_t key);
		uint32_t Query(std::span<uint8_t> output);
		uint32_t Register(uint32_t major, std::span<uint8_t> output);
		uint32_t Unregister(uint32_t session, uint32_t consumer);
		uint32_t Drain(uint32_t session, uint32_t consumer, std::span<uint8_t> output, uint32_t maxEvents);
		uint32_t Capture(uint32_t session, uint32_t consumer, uint32_t sequence, uint32_t flags, std::span<uint8_t> output);
		static bool BufferRange(uint32_t address, uint32_t bytes, uint32_t required);
		static bool KnownExport(const char* name);

	  private:
		uint64_t Now() const;
		void Expire(uint64_t now);
		void ReleaseCapture();
		void Reset(uint32_t reason);
		void Push(MCWInputEvent event);
		void Header(std::span<uint8_t> output, uint32_t count);
		bool Valid(uint32_t session, uint32_t consumer) const;
		uint32_t Modifiers() const;
		void UpdateRoute();
		Clock m_clock;
		std::mutex m_mutex;
		std::array<MCWInputEvent, MCW_INPUT_QUEUE_SIZE> m_events{};
		std::array<bool, 256> m_keys{}, m_blockedKeys{};
		uint32_t m_head{}, m_count{}, m_session{}, m_nextConsumer{}, m_consumer{}, m_sequence{};
		uint32_t m_captureSequence{}, m_capture{}, m_resetSerial{}, m_buttons{}, m_blockedButtons{};
		uint32_t m_neutralCapture{}, m_highSurrogate{};
		uint64_t m_lastDrain{};
		bool m_focused{};
		std::atomic_bool m_active{false};
		std::atomic_uint32_t m_route{0};
	};

	Provider& Get();
} // namespace guestinput
