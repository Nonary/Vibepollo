/* xlink wire protocol — shared by xlink-pi (Linux) and xlink-host (Windows).
 * UDP, little-endian. The pad-state payload mirrors ViGEm's XUSB_REPORT so the
 * host maps it 1:1 into a virtual Xbox 360 controller. */
#ifndef XLINK_PROTOCOL_H
#define XLINK_PROTOCOL_H

#include <stdint.h>

#define XLINK_MAGIC   0x4B4E4C58u /* "XLNK" */
#define XLINK_VERSION 1

enum xlink_type {
    XLINK_T_HELLO     = 1, /* pi->host: announce a session (kind, name) */
    XLINK_T_PAD_STATE = 2, /* pi->host: controller state snapshot       */
    XLINK_T_RUMBLE    = 3, /* host->pi: force-feedback motor levels     */
    XLINK_T_KEEPALIVE = 4, /* pi->host: idle heartbeat (~1 Hz)          */
    XLINK_T_BYE       = 5, /* pi->host: controller disconnected         */
};

#pragma pack(push, 1)

struct xlink_header {
    uint32_t magic;    /* XLINK_MAGIC   */
    uint8_t  version;  /* XLINK_VERSION */
    uint8_t  type;     /* enum xlink_type */
    uint16_t length;   /* payload bytes following the header */
    uint32_t seq;      /* per-sender monotonic sequence */
};

/* XLINK_T_PAD_STATE payload — identical field order to XUSB_REPORT. */
struct xlink_pad_state {
    uint16_t buttons;        /* XLINK_BTN_* bitmask */
    uint8_t  left_trigger;   /* 0..255 */
    uint8_t  right_trigger;  /* 0..255 */
    int16_t  lx, ly;         /* left stick,  -32768..32767 (Y up = positive) */
    int16_t  rx, ry;         /* right stick, -32768..32767 */
};

/* XLINK_T_RUMBLE payload */
struct xlink_rumble {
    uint8_t large_motor; /* 0..255 low-frequency */
    uint8_t small_motor; /* 0..255 high-frequency */
};

#pragma pack(pop)

/* Button bits — same values ViGEm/XUSB uses, so the host can memcpy buttons. */
#define XLINK_BTN_DPAD_UP        0x0001
#define XLINK_BTN_DPAD_DOWN      0x0002
#define XLINK_BTN_DPAD_LEFT      0x0004
#define XLINK_BTN_DPAD_RIGHT     0x0008
#define XLINK_BTN_START          0x0010
#define XLINK_BTN_BACK           0x0020
#define XLINK_BTN_LEFT_THUMB     0x0040
#define XLINK_BTN_RIGHT_THUMB    0x0080
#define XLINK_BTN_LEFT_SHOULDER  0x0100
#define XLINK_BTN_RIGHT_SHOULDER 0x0200
#define XLINK_BTN_GUIDE          0x0400
#define XLINK_BTN_A              0x1000
#define XLINK_BTN_B              0x2000
#define XLINK_BTN_X              0x4000
#define XLINK_BTN_Y              0x8000

#define XLINK_PAD_PORT 48200 /* UDP: controller state + rumble */
#define XLINK_MIC_PORT 48210 /* UDP: Opus/RTP mic audio */

#endif /* XLINK_PROTOCOL_H */
