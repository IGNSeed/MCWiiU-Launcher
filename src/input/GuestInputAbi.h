#pragma once

#include <stdint.h>
#include <stddef.h>

#define MCW_INPUT_MODULE "mcwiiu_input.rpl"
#define MCW_INPUT_MAGIC 0x4D435749u
#define MCW_INPUT_VERSION 0x00010001u
#define MCW_INPUT_HEADER_SIZE 64u
#define MCW_INPUT_EVENT_SIZE 48u
#define MCW_INPUT_MAX_DRAIN 32u
#define MCW_INPUT_QUEUE_SIZE 256u

enum MCWInputResult
{
	MCW_INPUT_OK = 0,
	MCW_INPUT_UNAVAILABLE = 1,
	MCW_INPUT_VERSION_MISMATCH = 2,
	MCW_INPUT_INVALID_BUFFER = 3,
	MCW_INPUT_INVALID_CONSUMER = 4,
	MCW_INPUT_BUSY = 5,
	MCW_INPUT_INVALID_REQUEST = 6
};

enum MCWInputCaptureFlags
{
	MCW_INPUT_CAPTURE_MOUSE = 1,
	MCW_INPUT_CAPTURE_KEYBOARD = 2,
	MCW_INPUT_CAPTURE_TEXT = 4
};

enum MCWInputEventType
{
	MCW_INPUT_RESET = 1,
	MCW_INPUT_MOVE = 2,
	MCW_INPUT_BUTTON = 3,
	MCW_INPUT_WHEEL = 4,
	MCW_INPUT_KEY = 5,
	MCW_INPUT_TEXT = 6,
	MCW_INPUT_FOCUS = 7,
	MCW_INPUT_LEAVE = 8,
	MCW_INPUT_CAPTURE_LOST = 9,
	MCW_INPUT_MENU_TOGGLE = 10
};

enum MCWInputCapabilities
{
	MCW_INPUT_CAP_MOUSE = 1,
	MCW_INPUT_CAP_KEYBOARD = 2,
	MCW_INPUT_CAP_UNICODE = 4,
	MCW_INPUT_CAP_CAPTURE = 8,
	MCW_INPUT_CAP_NORMALIZED = 16,
	MCW_INPUT_CAP_MENU_TOGGLE = 32
};

// 全fieldはGuest memory内でBig Endian。pointerやHost handleは含めない。
typedef struct MCWInputHeader
{
	uint32_t magic, version, headerSize, eventSize;
	uint32_t capabilities, session, consumer, captureSequence;
	uint32_t captureFlags, focused, sequence, count;
	uint32_t resetSerial, active, leaseMilliseconds, timestampFrequency;
} MCWInputHeader;

typedef struct MCWInputEvent
{
	uint32_t type, sequence, timestampHigh, timestampLow;
	uint32_t flags, display;
	int32_t x, y;
	uint32_t code, value, modifiers, reserved;
} MCWInputEvent;

#ifdef __cplusplus
static_assert(sizeof(MCWInputHeader) == 64 && alignof(MCWInputHeader) == 4);
static_assert(sizeof(MCWInputEvent) == 48 && alignof(MCWInputEvent) == 4);
static_assert(offsetof(MCWInputHeader, session) == 20 && offsetof(MCWInputHeader, count) == 44);
static_assert(offsetof(MCWInputEvent, x) == 24 && offsetof(MCWInputEvent, code) == 32);
extern "C"
{
#endif
	uint32_t MCWInputToggle(uint32_t session, uint32_t consumer, uint32_t enabled, void* output, uint32_t bytes);
	uint32_t MCWInputQuery(void* output, uint32_t bytes);
	uint32_t MCWInputRegister(uint32_t major, void* output, uint32_t bytes);
	uint32_t MCWInputUnregister(uint32_t session, uint32_t consumer);
	uint32_t MCWInputDrain(uint32_t session, uint32_t consumer, void* output, uint32_t bytes, uint32_t maxEvents);
	uint32_t MCWInputCapture(uint32_t session, uint32_t consumer, uint32_t sequence, uint32_t flags, void* output, uint32_t bytes);
#ifdef __cplusplus
}
#endif
