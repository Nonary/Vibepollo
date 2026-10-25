/**
 * @file src/steam_controller_protocol.h
 * @brief The Steam Controller (2026) extensions to the Moonlight protocol, in one place.
 * @details Vibepollo and the jlobue10 moonlight-common-c fork (the Android client) share these
 *          values; the moonlight-common-c submodule pinned here predates them, so each one is
 *          defined here as a fallback. A future submodule bump that defines a name with another
 *          value fails the `static_assert` below instead of silently masking the fallback: the
 *          values are wire-visible and the client fork's Limelight.h is the reference.
 */
#pragma once

#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>

#include <cstddef>

// The client type a Steam Controller arrives as.
#ifndef LI_CTYPE_STEAM
  #define LI_CTYPE_STEAM 0x04
#endif
static_assert(LI_CTYPE_STEAM == 0x04, "LI_CTYPE_STEAM disagrees with the client fork");

// Capability bits in the controller arrival event.
#ifndef LI_CCAP_GRIP_SENSE
  #define LI_CCAP_GRIP_SENSE 0x200  // Reports capacitive grip touch via LEFT/RIGHT_GRIP_TOUCH_FLAG
#endif
static_assert(LI_CCAP_GRIP_SENSE == 0x200, "LI_CCAP_GRIP_SENSE disagrees with the client fork");
#ifndef LI_CCAP_STICK_TOUCH
  #define LI_CCAP_STICK_TOUCH 0x400  // Reports capacitive stick touch via LEFT/RIGHT_STICK_TOUCH_FLAG
#endif
static_assert(LI_CCAP_STICK_TOUCH == 0x400, "LI_CCAP_STICK_TOUCH disagrees with the client fork");
#ifndef LI_CCAP_STEAM_HAPTIC
  #define LI_CCAP_STEAM_HAPTIC 0x800  // Replays Steam Controller haptic reports on the real pads
#endif
static_assert(LI_CCAP_STEAM_HAPTIC == 0x800, "LI_CCAP_STEAM_HAPTIC disagrees with the client fork");
#ifndef LI_CCAP_SC26_BUTTONS
  #define LI_CCAP_SC26_BUTTONS 0x1000  // Reports independent physical pad and trigger clicks (*_CLICK_FLAG)
#endif
static_assert(LI_CCAP_SC26_BUTTONS == 0x1000, "LI_CCAP_SC26_BUTTONS disagrees with the client fork");
#ifndef LI_CCAP_STEAM_SETTINGS
  #define LI_CCAP_STEAM_SETTINGS 0x2000  // Applies Steam Controller settings writes to the real controller
#endif
static_assert(LI_CCAP_STEAM_SETTINGS == 0x2000, "LI_CCAP_STEAM_SETTINGS disagrees with the client fork");

// Button flag bits above MISC_FLAG, in the upper half of the 32-bit button word
// (buttonFlags2 on the wire). Grip and stick touch are held, not pressed.
#ifndef LEFT_GRIP_TOUCH_FLAG
  #define LEFT_GRIP_TOUCH_FLAG 0x400000
#endif
static_assert(LEFT_GRIP_TOUCH_FLAG == 0x400000, "LEFT_GRIP_TOUCH_FLAG disagrees with the client fork");
#ifndef RIGHT_GRIP_TOUCH_FLAG
  #define RIGHT_GRIP_TOUCH_FLAG 0x800000
#endif
static_assert(RIGHT_GRIP_TOUCH_FLAG == 0x800000, "RIGHT_GRIP_TOUCH_FLAG disagrees with the client fork");
#ifndef LEFT_STICK_TOUCH_FLAG
  #define LEFT_STICK_TOUCH_FLAG 0x1000000
#endif
static_assert(LEFT_STICK_TOUCH_FLAG == 0x1000000, "LEFT_STICK_TOUCH_FLAG disagrees with the client fork");
#ifndef RIGHT_STICK_TOUCH_FLAG
  #define RIGHT_STICK_TOUCH_FLAG 0x2000000
#endif
static_assert(RIGHT_STICK_TOUCH_FLAG == 0x2000000, "RIGHT_STICK_TOUCH_FLAG disagrees with the client fork");
#ifndef LEFT_PAD_CLICK_FLAG
  #define LEFT_PAD_CLICK_FLAG 0x04000000
#endif
static_assert(LEFT_PAD_CLICK_FLAG == 0x04000000, "LEFT_PAD_CLICK_FLAG disagrees with the client fork");
#ifndef RIGHT_PAD_CLICK_FLAG
  #define RIGHT_PAD_CLICK_FLAG 0x08000000
#endif
static_assert(RIGHT_PAD_CLICK_FLAG == 0x08000000, "RIGHT_PAD_CLICK_FLAG disagrees with the client fork");
#ifndef LEFT_TRIGGER_CLICK_FLAG
  #define LEFT_TRIGGER_CLICK_FLAG 0x10000000
#endif
static_assert(LEFT_TRIGGER_CLICK_FLAG == 0x10000000, "LEFT_TRIGGER_CLICK_FLAG disagrees with the client fork");
#ifndef RIGHT_TRIGGER_CLICK_FLAG
  #define RIGHT_TRIGGER_CLICK_FLAG 0x20000000
#endif
static_assert(RIGHT_TRIGGER_CLICK_FLAG == 0x20000000, "RIGHT_TRIGGER_CLICK_FLAG disagrees with the client fork");

// A haptic output report (0x80 rumble, 0x81 pulse, 0x82 command, 0x83 LFO tone, 0x84 log
// sweep, 0x85 script) forwarded verbatim, id first, to a LI_CCAP_STEAM_HAPTIC client.
#ifndef STEAM_HAPTIC_REPORT_MAX
  #define STEAM_HAPTIC_REPORT_MAX 12
#endif
static_assert(STEAM_HAPTIC_REPORT_MAX == 12, "STEAM_HAPTIC_REPORT_MAX disagrees with the client fork");

// A settings write forwarded to a LI_CCAP_STEAM_SETTINGS client: SET_SETTINGS_VALUES with
// 1..STEAM_SETTINGS_TRIPLES_MAX (id, lo, hi) triples, or LOAD_DEFAULT_SETTINGS with none.
#ifndef STEAM_SETTINGS_TRIPLES_MAX
  #define STEAM_SETTINGS_TRIPLES_MAX 9
#endif
static_assert(STEAM_SETTINGS_TRIPLES_MAX == 9, "STEAM_SETTINGS_TRIPLES_MAX disagrees with the client fork");
#ifndef STEAM_SETTINGS_PAYLOAD_SIZE
  #define STEAM_SETTINGS_PAYLOAD_SIZE (STEAM_SETTINGS_TRIPLES_MAX * 3)
#endif
static_assert(STEAM_SETTINGS_PAYLOAD_SIZE == 27, "STEAM_SETTINGS_PAYLOAD_SIZE disagrees with the client fork");
#ifndef STEAM_SETTINGS_SET_VALUES
  #define STEAM_SETTINGS_SET_VALUES 0x87
#endif
static_assert(STEAM_SETTINGS_SET_VALUES == 0x87, "STEAM_SETTINGS_SET_VALUES disagrees with the client fork");
#ifndef STEAM_SETTINGS_LOAD_DEFAULTS
  #define STEAM_SETTINGS_LOAD_DEFAULTS 0x8E
#endif
static_assert(STEAM_SETTINGS_LOAD_DEFAULTS == 0x8E, "STEAM_SETTINGS_LOAD_DEFAULTS disagrees with the client fork");

// Control stream packet types carrying the two (Vibepollo protocol extension). The client
// fork keeps them in ControlStream.c's packetTypes[] table rather than a header, so these
// names are Vibepollo's; the asserts still catch a header that grows them with another value.
#ifndef STEAM_HAPTIC_PACKET_TYPE
  #define STEAM_HAPTIC_PACKET_TYPE 0x5504
#endif
static_assert(STEAM_HAPTIC_PACKET_TYPE == 0x5504, "STEAM_HAPTIC_PACKET_TYPE disagrees with the client fork");
#ifndef STEAM_SETTINGS_PACKET_TYPE
  #define STEAM_SETTINGS_PACKET_TYPE 0x5505
#endif
static_assert(STEAM_SETTINGS_PACKET_TYPE == 0x5505, "STEAM_SETTINGS_PACKET_TYPE disagrees with the client fork");

// The real Steam Controller's firmware attributes (GET_ATTRIBUTES_VALUES, GET_DEVICE_INFO and the
// two serial strings), read by the client at link setup and relayed to the virtual controller so
// Steam sees the real firmware build and only offers an update the real controller needs. The
// capability bit says the client sends them; the LI_STEAM_CONTROLLER_ATTRIBUTES struct is the
// client's API shape, the SS_ packet is what crosses the wire (every u32 little-endian).
#ifndef LI_CCAP_STEAM_ATTRIBUTES
  #define LI_CCAP_STEAM_ATTRIBUTES 0x4000  // Sends the real Steam Controller's firmware attributes

  // Which fields of LI_STEAM_CONTROLLER_ATTRIBUTES carry a value read from the controller
  #define LI_SCATTR_FIRMWARE_BUILD_TIME 0x01
  #define LI_SCATTR_BOOTLOADER_BUILD_TIME 0x02
  #define LI_SCATTR_BOARD_REVISION 0x04
  #define LI_SCATTR_CAPABILITIES 0x08
  #define LI_SCATTR_RADIO_FW_BUILD_TIME 0x10
  #define LI_SCATTR_DONGLE_FW_BUILD_TIME 0x20
  #define LI_SCATTR_BUILD_ID 0x40
  #define LI_SCATTR_SERIALS 0x80

  // How the client is connected to the real unit (informational)
  #define LI_SCATTR_TRANSPORT_USB 0
  #define LI_SCATTR_TRANSPORT_BLE 1
  #define LI_SCATTR_TRANSPORT_PUCK 2

typedef struct _LI_STEAM_CONTROLLER_ATTRIBUTES {
  uint8_t transport;  // LI_SCATTR_TRANSPORT_*
  uint8_t validMask;  // LI_SCATTR_* bits
  uint8_t reserved[2];  // zero
  uint32_t firmwareBuildTime;  // GET_ATTRIBUTES tag 4 (unix seconds)
  uint32_t bootloaderBuildTime;  // tag 10
  uint32_t boardRevision;  // tag 9
  uint32_t capabilities;  // tag 2
  uint32_t radioFirmwareBuildTime;  // tag 5 (0 if absent)
  uint32_t dongleFirmwareBuildTime;  // tag 8 (0 if absent; puck only)
  char buildId[16];  // GET_DEVICE_INFO sub 0 build id (12 ASCII chars), NUL padded
  char unitSerial[16];  // GET_STRING_ATTRIBUTE tag 1 ("FXA..."), NUL padded
  char boardSerial[16];  // GET_STRING_ATTRIBUTE tag 0 ("MXA..."), NUL padded
} LI_STEAM_CONTROLLER_ATTRIBUTES, *PLI_STEAM_CONTROLLER_ATTRIBUTES;
#endif
static_assert(LI_CCAP_STEAM_ATTRIBUTES == 0x4000, "LI_CCAP_STEAM_ATTRIBUTES disagrees with the client fork");
static_assert(LI_SCATTR_FIRMWARE_BUILD_TIME == 0x01, "LI_SCATTR_FIRMWARE_BUILD_TIME disagrees with the client fork");
static_assert(LI_SCATTR_BOOTLOADER_BUILD_TIME == 0x02, "LI_SCATTR_BOOTLOADER_BUILD_TIME disagrees with the client fork");
static_assert(LI_SCATTR_BOARD_REVISION == 0x04, "LI_SCATTR_BOARD_REVISION disagrees with the client fork");
static_assert(LI_SCATTR_CAPABILITIES == 0x08, "LI_SCATTR_CAPABILITIES disagrees with the client fork");
static_assert(LI_SCATTR_RADIO_FW_BUILD_TIME == 0x10, "LI_SCATTR_RADIO_FW_BUILD_TIME disagrees with the client fork");
static_assert(LI_SCATTR_DONGLE_FW_BUILD_TIME == 0x20, "LI_SCATTR_DONGLE_FW_BUILD_TIME disagrees with the client fork");
static_assert(LI_SCATTR_BUILD_ID == 0x40, "LI_SCATTR_BUILD_ID disagrees with the client fork");
static_assert(LI_SCATTR_SERIALS == 0x80, "LI_SCATTR_SERIALS disagrees with the client fork");
static_assert(LI_SCATTR_TRANSPORT_USB == 0, "LI_SCATTR_TRANSPORT_USB disagrees with the client fork");
static_assert(LI_SCATTR_TRANSPORT_BLE == 1, "LI_SCATTR_TRANSPORT_BLE disagrees with the client fork");
static_assert(LI_SCATTR_TRANSPORT_PUCK == 2, "LI_SCATTR_TRANSPORT_PUCK disagrees with the client fork");
static_assert(sizeof(LI_STEAM_CONTROLLER_ATTRIBUTES) == 76, "LI_STEAM_CONTROLLER_ATTRIBUTES disagrees with the client fork");
static_assert(offsetof(LI_STEAM_CONTROLLER_ATTRIBUTES, firmwareBuildTime) == 4, "LI_STEAM_CONTROLLER_ATTRIBUTES disagrees with the client fork");
static_assert(offsetof(LI_STEAM_CONTROLLER_ATTRIBUTES, buildId) == 28, "LI_STEAM_CONTROLLER_ATTRIBUTES disagrees with the client fork");

// The input packet carrying them (client fork Input.h), ordered after the arrival event on the
// same reliable channel. Packed like every other packet in that header.
#ifndef SS_STEAM_CONTROLLER_ATTRIBUTES_MAGIC
  #define SS_STEAM_CONTROLLER_ATTRIBUTES_MAGIC 0x55000008
  #pragma pack(push, 1)
typedef struct _SS_STEAM_CONTROLLER_ATTRIBUTES_PACKET {
  NV_INPUT_HEADER header;
  uint8_t controllerNumber;
  uint8_t transport;
  uint8_t validMask;
  uint8_t reserved;
  uint32_t firmwareBuildTime;  // all u32 LE32 on the wire, like the arrival packet
  uint32_t bootloaderBuildTime;
  uint32_t boardRevision;
  uint32_t capabilities;
  uint32_t radioFirmwareBuildTime;
  uint32_t dongleFirmwareBuildTime;
  char buildId[16];
  char unitSerial[16];
  char boardSerial[16];
} SS_STEAM_CONTROLLER_ATTRIBUTES_PACKET, *PSS_STEAM_CONTROLLER_ATTRIBUTES_PACKET;
  #pragma pack(pop)
#endif
static_assert(SS_STEAM_CONTROLLER_ATTRIBUTES_MAGIC == 0x55000008, "SS_STEAM_CONTROLLER_ATTRIBUTES_MAGIC disagrees with the client fork");
static_assert(sizeof(SS_STEAM_CONTROLLER_ATTRIBUTES_PACKET) == sizeof(NV_INPUT_HEADER) + 4 + 24 + 48, "SS_STEAM_CONTROLLER_ATTRIBUTES_PACKET disagrees with the client fork");
static_assert(offsetof(SS_STEAM_CONTROLLER_ATTRIBUTES_PACKET, firmwareBuildTime) == sizeof(NV_INPUT_HEADER) + 4, "SS_STEAM_CONTROLLER_ATTRIBUTES_PACKET disagrees with the client fork");
static_assert(offsetof(SS_STEAM_CONTROLLER_ATTRIBUTES_PACKET, buildId) == sizeof(NV_INPUT_HEADER) + 28, "SS_STEAM_CONTROLLER_ATTRIBUTES_PACKET disagrees with the client fork");
static_assert(offsetof(SS_STEAM_CONTROLLER_ATTRIBUTES_PACKET, boardSerial) == sizeof(NV_INPUT_HEADER) + 60, "SS_STEAM_CONTROLLER_ATTRIBUTES_PACKET disagrees with the client fork");
