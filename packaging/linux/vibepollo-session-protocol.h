#ifndef VIBEPOLLO_SESSION_PROTOCOL_H
#define VIBEPOLLO_SESSION_PROTOCOL_H

#include <stdint.h>

#define VIBEPOLLO_SESSION_BROKER_SOCKET "/run/vibepollo/session-broker.sock"
#define VIBEPOLLO_SESSION_PROTOCOL_MAGIC UINT32_C(0x56534252)
#define VIBEPOLLO_SESSION_PROTOCOL_VERSION UINT16_C(1)
#define VIBEPOLLO_SESSION_PROTOCOL_MAX_ARGUMENTS UINT32_C(65)
#define VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE UINT32_C(131072)
#define VIBEPOLLO_SESSION_PROTOCOL_OUTPUT_CHUNK UINT32_C(16384)

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

_Static_assert(sizeof(struct vibepollo_session_message) == 32,
               "session broker protocol header must remain fixed-size");

#endif
