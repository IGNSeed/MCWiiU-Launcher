#include "GuestInput.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

namespace guestinput
{
	namespace
	{
		constexpr uint32_t CAPABILITIES = 63;
		constexpr uint32_t RIGHT_SHIFT = 0xA1;
		constexpr uint32_t TOGGLE_ROUTE = 8;
		constexpr uint64_t LEASE_MS = 2000;

		uint64_t ClockMilliseconds()
		{
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
											 std::chrono::steady_clock::now().time_since_epoch())
											 .count());
		}

		void Word(std::span<uint8_t> output, size_t offset, uint32_t value)
		{
			output[offset] = static_cast<uint8_t>(value >> 24);
			output[offset + 1] = static_cast<uint8_t>(value >> 16);
			output[offset + 2] = static_cast<uint8_t>(value >> 8);
			output[offset + 3] = static_cast<uint8_t>(value);
		}

		void Encode(std::span<uint8_t> output, const MCWInputEvent& event)
		{
			const uint32_t words[] = {event.type, event.sequence, event.timestampHigh, event.timestampLow,
									  event.flags, event.display, static_cast<uint32_t>(event.x), static_cast<uint32_t>(event.y),
									  event.code, event.value, event.modifiers, 0};
			for (size_t i = 0; i < std::size(words); ++i)
			{
				Word(output, i * 4, words[i]);
			}
		}
	} // namespace

	Provider::Provider(Clock clock) : m_clock(clock ? clock : ClockMilliseconds) {}

	uint64_t Provider::Now() const
	{
		return m_clock();
	}

	Provider& Get()
	{
		static Provider provider;
		return provider;
	}

	void Provider::UpdateRoute()
	{
		uint32_t route = m_capture | (m_toggle ? TOGGLE_ROUTE : 0u) | (m_blockedButtons ? MCW_INPUT_CAPTURE_MOUSE : 0u);
		if (std::any_of(m_blockedKeys.begin(), m_blockedKeys.end(), [](bool down) { return down; }))
		{
			route |= MCW_INPUT_CAPTURE_KEYBOARD;
		}
		m_route.store(route, std::memory_order_release);
	}

	void Provider::ReleaseCapture()
	{
		m_blockedButtons |= m_buttons;
		for (size_t i = 0; i < m_keys.size(); ++i)
		{
			m_blockedKeys[i] = m_blockedKeys[i] || m_keys[i];
		}
		m_capture = m_neutralCapture = m_highSurrogate = 0;
		UpdateRoute();
	}

	void Provider::ReleaseToggle()
	{
		if (m_toggle && m_keys[RIGHT_SHIFT])
			m_blockedKeys[RIGHT_SHIFT] = true;
		m_toggle = m_toggleArmed = false;
		UpdateRoute();
	}

	void Provider::Reset(uint32_t reason)
	{
		m_head = m_count = 0;
		++m_resetSerial;
		MCWInputEvent event{};
		event.type = MCW_INPUT_RESET;
		event.value = reason;
		Push(event);
	}

	void Provider::Expire(uint64_t now)
	{
		if (m_consumer && (now < m_lastDrain || now - m_lastDrain > LEASE_MS))
		{
			ReleaseCapture();
			ReleaseToggle();
			m_consumer = 0;
			m_head = m_count = 0;
			++m_resetSerial;
		}
	}

	void Provider::BeginSession()
	{
		std::lock_guard lock(m_mutex);
		m_active = false;
		ReleaseCapture();
		ReleaseToggle();
		m_consumer = m_captureSequence = m_sequence = m_head = m_count = 0;
		m_keys.fill(false);
		m_blockedKeys.fill(false);
		m_buttons = m_blockedButtons = 0;
		m_focused = false;
		if (m_session != std::numeric_limits<uint32_t>::max())
		{
			++m_session;
			m_active = true;
		}
		UpdateRoute();
	}

	void Provider::EndSession()
	{
		std::lock_guard lock(m_mutex);
		m_active = false;
		ReleaseCapture();
		ReleaseToggle();
		m_consumer = m_head = m_count = 0;
		m_keys.fill(false);
		m_blockedKeys.fill(false);
		m_buttons = m_blockedButtons = 0;
		m_focused = false;
		UpdateRoute();
	}

	bool Provider::Available() const
	{
		return m_active.load(std::memory_order_acquire);
	}

	bool Provider::BufferRange(uint32_t address, uint32_t bytes, uint32_t required)
	{
		return address != 0 && (address & 3) == 0 && required >= MCW_INPUT_HEADER_SIZE &&
			   bytes >= required && bytes <= MCW_INPUT_HEADER_SIZE + MCW_INPUT_MAX_DRAIN * MCW_INPUT_EVENT_SIZE &&
			   address <= std::numeric_limits<uint32_t>::max() - bytes;
	}

	bool Provider::KnownExport(const char* name)
	{
		if (!name)
			return false;
		for (const char* known : {"MCWInputQuery", "MCWInputRegister", "MCWInputUnregister", "MCWInputDrain", "MCWInputCapture", "MCWInputToggle"})
		{
			if (std::strcmp(name, known) == 0)
				return true;
		}
		return false;
	}

	bool Provider::Valid(uint32_t session, uint32_t consumer) const
	{
		return m_active && session == m_session && consumer != 0 && consumer == m_consumer;
	}

	void Provider::Push(MCWInputEvent event)
	{
		if (!m_consumer)
			return;
		if (m_sequence == std::numeric_limits<uint32_t>::max())
		{
			ReleaseCapture();
			ReleaseToggle();
			m_consumer = m_count = 0;
			return;
		}
		event.sequence = ++m_sequence;
		const uint64_t ticks = Now();
		event.timestampHigh = static_cast<uint32_t>(ticks >> 32);
		event.timestampLow = static_cast<uint32_t>(ticks);
		if (m_count && event.type == MCW_INPUT_MOVE)
		{
			auto& last = m_events[(m_head + m_count - 1) % MCW_INPUT_QUEUE_SIZE];
			if (last.type == MCW_INPUT_MOVE && last.display == event.display)
			{
				last = event;
				return;
			}
		}
		if (m_count == MCW_INPUT_QUEUE_SIZE)
		{
			ReleaseCapture();
			ReleaseToggle();
			m_head = m_count = 0;
			++m_resetSerial;
			event = {};
			event.type = MCW_INPUT_RESET;
			event.sequence = m_sequence;
			event.value = 1; // overflow: Guestは全pressed状態を破棄する
		}
		m_events[(m_head + m_count++) % MCW_INPUT_QUEUE_SIZE] = event;
	}

	void Provider::Header(std::span<uint8_t> output, uint32_t count)
	{
		const uint32_t words[] = {MCW_INPUT_MAGIC, MCW_INPUT_VERSION, MCW_INPUT_HEADER_SIZE, MCW_INPUT_EVENT_SIZE,
								  CAPABILITIES, m_session, m_consumer, m_captureSequence, m_capture, m_focused ? 1u : 0u,
								  m_sequence, count, m_resetSerial, m_active ? 1u : 0u, static_cast<uint32_t>(LEASE_MS), 1000};
		for (size_t i = 0; i < std::size(words); ++i)
			Word(output, i * 4, words[i]);
	}

	uint32_t Provider::Toggle(uint32_t session, uint32_t consumer, uint32_t enabled, std::span<uint8_t> output)
	{
		if (output.size() < MCW_INPUT_HEADER_SIZE)
			return MCW_INPUT_INVALID_BUFFER;
		if (enabled > 1)
			return MCW_INPUT_INVALID_REQUEST;
		std::lock_guard lock(m_mutex);
		Expire(Now());
		if (!Valid(session, consumer))
			return MCW_INPUT_INVALID_CONSUMER;
		if (enabled && !m_focused)
			return MCW_INPUT_INVALID_REQUEST;
		if (!enabled)
			ReleaseToggle();
		else if (!m_toggle)
		{
			m_toggle = true;
			m_toggleArmed = !m_keys[RIGHT_SHIFT] && !m_blockedKeys[RIGHT_SHIFT];
		}
		m_lastDrain = Now();
		UpdateRoute();
		Header(output, 0);
		return MCW_INPUT_OK;
	}

	uint32_t Provider::Query(std::span<uint8_t> output)
	{
		if (output.size() < MCW_INPUT_HEADER_SIZE)
			return MCW_INPUT_INVALID_BUFFER;
		std::lock_guard lock(m_mutex);
		Expire(Now());
		Header(output, 0);
		return m_active ? MCW_INPUT_OK : MCW_INPUT_UNAVAILABLE;
	}

	uint32_t Provider::Register(uint32_t major, std::span<uint8_t> output)
	{
		if (output.size() < MCW_INPUT_HEADER_SIZE)
			return MCW_INPUT_INVALID_BUFFER;
		if (major != 1)
			return MCW_INPUT_VERSION_MISMATCH;
		std::lock_guard lock(m_mutex);
		Expire(Now());
		if (!m_active)
			return MCW_INPUT_UNAVAILABLE;
		if (m_consumer)
			return MCW_INPUT_BUSY;
		if (m_nextConsumer == std::numeric_limits<uint32_t>::max())
			return MCW_INPUT_UNAVAILABLE;
		m_consumer = ++m_nextConsumer;
		m_captureSequence = 0;
		m_lastDrain = Now();
		Reset(0);
		Header(output, 0);
		return MCW_INPUT_OK;
	}

	uint32_t Provider::Unregister(uint32_t session, uint32_t consumer)
	{
		std::lock_guard lock(m_mutex);
		Expire(Now());
		if (!Valid(session, consumer))
			return MCW_INPUT_INVALID_CONSUMER;
		ReleaseCapture();
		ReleaseToggle();
		m_consumer = m_count = m_head = 0;
		return MCW_INPUT_OK;
	}

	uint32_t Provider::Drain(uint32_t session, uint32_t consumer, std::span<uint8_t> output, uint32_t maxEvents)
	{
		if (maxEvents == 0 || maxEvents > MCW_INPUT_MAX_DRAIN ||
			output.size() < MCW_INPUT_HEADER_SIZE + maxEvents * MCW_INPUT_EVENT_SIZE)
			return MCW_INPUT_INVALID_BUFFER;
		std::lock_guard lock(m_mutex);
		Expire(Now());
		if (!Valid(session, consumer))
			return MCW_INPUT_INVALID_CONSUMER;
		m_lastDrain = Now();
		const uint32_t count = std::min(maxEvents, m_count);
		Header(output, count);
		for (uint32_t i = 0; i < count; ++i)
		{
			Encode(output.subspan(MCW_INPUT_HEADER_SIZE + i * MCW_INPUT_EVENT_SIZE, MCW_INPUT_EVENT_SIZE), m_events[m_head]);
			m_head = (m_head + 1) % MCW_INPUT_QUEUE_SIZE;
			--m_count;
		}
		return MCW_INPUT_OK;
	}

	uint32_t Provider::Capture(uint32_t session, uint32_t consumer, uint32_t sequence, uint32_t flags, std::span<uint8_t> output)
	{
		if (output.size() < MCW_INPUT_HEADER_SIZE)
			return MCW_INPUT_INVALID_BUFFER;
		if ((flags & ~7u) || ((flags & MCW_INPUT_CAPTURE_TEXT) && !(flags & MCW_INPUT_CAPTURE_KEYBOARD)))
			return MCW_INPUT_INVALID_REQUEST;
		std::lock_guard lock(m_mutex);
		Expire(Now());
		if (!Valid(session, consumer))
			return MCW_INPUT_INVALID_CONSUMER;
		if (sequence == 0 || sequence <= m_captureSequence || (flags && !m_focused))
			return MCW_INPUT_INVALID_REQUEST;
		if (flags == m_capture)
		{
			m_captureSequence = sequence;
			m_lastDrain = Now();
			Header(output, 0);
			return MCW_INPUT_OK;
		}
		ReleaseCapture();
		m_captureSequence = sequence;
		m_capture = flags;
		m_lastDrain = Now();
		if ((flags & MCW_INPUT_CAPTURE_MOUSE) && m_buttons)
			m_neutralCapture |= MCW_INPUT_CAPTURE_MOUSE;
		if ((flags & MCW_INPUT_CAPTURE_KEYBOARD) &&
			std::any_of(m_keys.begin(), m_keys.end(), [](bool down) { return down; }))
			m_neutralCapture |= MCW_INPUT_CAPTURE_KEYBOARD;
		UpdateRoute();
		Reset(2);
		Header(output, 0);
		return MCW_INPUT_OK;
	}

	void Provider::Focus(bool focused)
	{
		if (!Available())
			return;
		std::lock_guard lock(m_mutex);
		Expire(Now());
		if (m_focused == focused)
			return;
		m_focused = focused;
		if (!focused)
		{
			ReleaseCapture();
			ReleaseToggle();
			m_keys.fill(false);
			m_buttons = 0;
			Reset(3);
		}
		MCWInputEvent event{};
		event.type = MCW_INPUT_FOCUS;
		event.value = focused ? 1 : 0;
		Push(event);
	}

	uint32_t Provider::Modifiers() const
	{
		return (m_keys[0xA0] || (!m_toggle && m_keys[RIGHT_SHIFT]) ? 1u : 0u) |
			   (m_keys[0xA2] || m_keys[0xA3] ? 2u : 0u) |
			   (m_keys[0xA4] || m_keys[0xA5] ? 4u : 0u) |
			   (m_keys[0x5B] || m_keys[0x5C] ? 8u : 0u);
	}

	void Provider::Pointer(uint32_t type, int32_t x, int32_t y, const Viewport& viewport,
						   uint32_t button, bool down, int32_t wheelX, int32_t wheelY)
	{
		if (!Available())
			return;
		std::lock_guard lock(m_mutex);
		Expire(Now());
		if (!m_focused || viewport.display > 1)
			return;
		const bool inside = viewport.width > 0 && viewport.height > 0 &&
							static_cast<int64_t>(x) >= viewport.x && static_cast<int64_t>(y) >= viewport.y &&
							static_cast<int64_t>(x) < static_cast<int64_t>(viewport.x) + viewport.width &&
							static_cast<int64_t>(y) < static_cast<int64_t>(viewport.y) + viewport.height;
		const uint32_t before = m_buttons;
		if (type == MCW_INPUT_BUTTON)
		{
			if (button > 2)
				return;
			const uint32_t mask = 1u << button;
			if (down && !inside && !before)
				return;
			if (down)
				m_buttons |= mask;
			else
				m_buttons &= ~mask;
			if (!down)
				m_blockedButtons &= ~mask;
			if (before == m_buttons)
				return;
		}
		else if (type == MCW_INPUT_LEAVE || type == MCW_INPUT_CAPTURE_LOST)
		{
			if (type == MCW_INPUT_CAPTURE_LOST)
			{
				ReleaseCapture();
				ReleaseToggle();
				m_buttons = 0;
				Reset(4);
			}
		}
		else if (type != MCW_INPUT_MOVE && type != MCW_INPUT_WHEEL)
			return;
		if (!m_buttons)
			m_neutralCapture &= ~MCW_INPUT_CAPTURE_MOUSE;
		UpdateRoute();
		if (!m_consumer || (m_neutralCapture & MCW_INPUT_CAPTURE_MOUSE))
			return;
		if (!inside && !before && type != MCW_INPUT_LEAVE && type != MCW_INPUT_CAPTURE_LOST)
			return;
		MCWInputEvent event{};
		event.type = type;
		event.flags = inside ? 1 : 0;
		event.display = viewport.display;
		if (viewport.width > 0 && viewport.height > 0)
		{
			const auto q16 = [](int32_t position, int32_t origin, int32_t extent) {
				return static_cast<int32_t>(std::clamp<int64_t>(
					(static_cast<int64_t>(position) - origin) * 65536 / extent, INT32_MIN, INT32_MAX));
			};
			event.x = q16(x, viewport.x, viewport.width);
			event.y = q16(y, viewport.y, viewport.height);
		}
		if (type == MCW_INPUT_WHEEL)
		{
			event.code = static_cast<uint32_t>(wheelX);
			event.value = static_cast<uint32_t>(wheelY);
		}
		else
		{
			event.code = button;
			event.value = m_buttons;
		}
		Push(event);
	}

	void Provider::Key(uint32_t key, bool down, bool repeat)
	{
		if (!Available() || key >= 256)
			return;
		std::lock_guard lock(m_mutex);
		Expire(Now());
		if (!m_focused)
			return;
		const bool reserved = m_toggle && m_consumer && key == RIGHT_SHIFT;
		const bool previous = m_keys[key];
		m_keys[key] = down;
		if (!down)
			m_blockedKeys[key] = false;
		if (!std::any_of(m_keys.begin(), m_keys.end(), [](bool held) { return held; }))
			m_neutralCapture &= ~MCW_INPUT_CAPTURE_KEYBOARD;
		UpdateRoute();
		if (reserved)
		{
			if (!down)
				m_toggleArmed = true;
			else if (!previous && !repeat && m_toggleArmed)
			{
				m_toggleArmed = false;
				MCWInputEvent event{};
				event.type = MCW_INPUT_MENU_TOGGLE;
				event.code = RIGHT_SHIFT;
				event.value = 1;
				Push(event);
			}
			return;
		}
		if (!m_consumer || !(m_capture & MCW_INPUT_CAPTURE_KEYBOARD) ||
			(m_neutralCapture & MCW_INPUT_CAPTURE_KEYBOARD) || previous == down)
			return;
		MCWInputEvent event{};
		event.type = MCW_INPUT_KEY;
		event.code = key;
		event.value = down ? 1 : 0;
		event.flags = repeat ? 1 : 0;
		event.modifiers = Modifiers();
		Push(event);
	}

	void Provider::Character(uint32_t utf16)
	{
		if (!Available())
			return;
		std::lock_guard lock(m_mutex);
		Expire(Now());
		if (!m_consumer || !m_focused || !(m_capture & MCW_INPUT_CAPTURE_TEXT) ||
			(m_neutralCapture & MCW_INPUT_CAPTURE_KEYBOARD))
		{
			m_highSurrogate = 0;
			return;
		}
		if (utf16 > 0xFFFF)
		{
			m_highSurrogate = 0;
			return;
		}
		uint32_t scalar = utf16;
		if (utf16 >= 0xD800 && utf16 <= 0xDBFF)
		{
			m_highSurrogate = utf16;
			return;
		}
		if (utf16 >= 0xDC00 && utf16 <= 0xDFFF)
		{
			if (!m_highSurrogate)
				return;
			scalar = 0x10000 + ((m_highSurrogate - 0xD800) << 10) + utf16 - 0xDC00;
		}
		m_highSurrogate = 0;
		if (scalar < 32 || scalar == 127 || scalar > 0x10FFFF)
			return;
		MCWInputEvent event{};
		event.type = MCW_INPUT_TEXT;
		event.value = scalar;
		event.modifiers = Modifiers();
		Push(event);
	}

	Route Provider::LegacyRoute()
	{
		if (!m_route.load(std::memory_order_acquire))
			return {};
		std::lock_guard lock(m_mutex);
		Expire(Now());
		return {(m_capture & MCW_INPUT_CAPTURE_MOUSE) != 0 || m_blockedButtons != 0,
				(m_capture & MCW_INPUT_CAPTURE_KEYBOARD) != 0};
	}

	bool Provider::LegacyKeyAllowed(uint32_t key)
	{
		if (!m_route.load(std::memory_order_acquire))
			return true;
		std::lock_guard lock(m_mutex);
		Expire(Now());
		return !(m_capture & MCW_INPUT_CAPTURE_KEYBOARD) &&
			   !(m_toggle && m_focused && m_consumer && key == RIGHT_SHIFT) &&
			   (key >= 256 || !m_blockedKeys[key]);
	}
} // namespace guestinput
