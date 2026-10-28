#ifndef VIBEPOLLO_SESSION_PROTOCOL_H
#define VIBEPOLLO_SESSION_PROTOCOL_H

#include <stdint.h>

#define VIBEPOLLO_SESSION_BROKER_SOCKET "/run/vibepollo/session-broker.sock"
#define VIBEPOLLO_SESSION_PROTOCOL_MAGIC UINT32_C(0x56534252)
#define VIBEPOLLO_SESSION_PROTOCOL_VERSION UINT16_C(1)
/* Complete activation/restore transactions need up to seven properties for
 * each output in the bounded 64-output topology, plus the operation verb.
 * The independent message byte limit still applies to every request. */
#define VIBEPOLLO_SESSION_PROTOCOL_MAX_DISPLAY_PROPERTIES (UINT32_C(64) * UINT32_C(7))
#define VIBEPOLLO_SESSION_PROTOCOL_MAX_ARGUMENTS (VIBEPOLLO_SESSION_PROTOCOL_MAX_DISPLAY_PROPERTIES + UINT32_C(1))
#define VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE UINT32_C(131072)
#define VIBEPOLLO_SESSION_PROTOCOL_OUTPUT_CHUNK UINT32_C(16384)
/* No terminal broker acknowledgement was received; mutation completion is unknown. */
#define VIBEPOLLO_SESSION_COMPLETION_UNKNOWN 125
/* KScreen requests are cancelled and reaped by the broker before reporting exit. */
#define VIBEPOLLO_SESSION_DISPLAY_TIMEOUT_MS UINT64_C(5000)

enum vibepollo_session_message_type {
  VIBEPOLLO_SESSION_REQUEST = 1,
  VIBEPOLLO_SESSION_STDOUT = 2,
  VIBEPOLLO_SESSION_STDERR = 3,
  VIBEPOLLO_SESSION_EXIT = 4,
};

/*
 * This protocol is local to one machine and intentionally uses native byte
 * order. Fixed-width fields and an exact version still make every message
 * self-describing and reject ABI or implementation drift instead of silently
 * reinterpreting it.
 */
struct vibepollo_session_message {
  uint32_t magic;
  uint16_t version;
  uint16_t type;
  uint32_t payload_length;
  uint32_t argument_count;
  uint64_t generation;
  int32_t status;
  uint32_t reserved;
};

#ifdef __cplusplus
static_assert(sizeof(struct vibepollo_session_message) == 32,
#else
_Static_assert(sizeof(struct vibepollo_session_message) == 32,
#endif
               "session broker protocol header must remain fixed-size");

#endif
