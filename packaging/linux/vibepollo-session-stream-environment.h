#ifndef VIBEPOLLO_SESSION_STREAM_ENVIRONMENT_H
#define VIBEPOLLO_SESSION_STREAM_ENVIRONMENT_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Application requests may carry these stream-owned values as literal argv
// data. They never become the capability-bearing broker's environment. The
// broker validates the complete list before dropping identity, then supplies
// it only to the selected user's transient application service.
#define VIBEPOLLO_STREAM_ENVIRONMENT_MAX_ENTRIES 32U
#define VIBEPOLLO_STREAM_ENVIRONMENT_MAX_BYTES 16384U

enum vibepollo_stream_environment_type {
  VIBEPOLLO_STREAM_TEXT,
  VIBEPOLLO_STREAM_UNSIGNED,
  VIBEPOLLO_STREAM_SIGNED,
  VIBEPOLLO_STREAM_DECIMAL,
  VIBEPOLLO_STREAM_BOOLEAN,
  VIBEPOLLO_STREAM_AUDIO_LAYOUT,
  VIBEPOLLO_STREAM_APP_STATUS,
  VIBEPOLLO_STREAM_TOGGLE
};

struct vibepollo_stream_environment_field {
  const char *name;
  enum vibepollo_stream_environment_type type;
  size_t maximum_length;
};

// This is an exact allowlist, not a SUNSHINE_/APOLLO_ prefix match. In
// particular PATH, HOME, LD_PRELOAD and manager/session settings stay under
// broker policy. ENABLE_HDR_WSI remains the fixed app-wayland-hdr operation.
static const struct vibepollo_stream_environment_field vibepollo_stream_environment_fields[] = {
  {"SUNSHINE_APP_ID", VIBEPOLLO_STREAM_UNSIGNED, 10},
  {"SUNSHINE_APP_NAME", VIBEPOLLO_STREAM_TEXT, 4096},
  {"SUNSHINE_CLIENT_WIDTH", VIBEPOLLO_STREAM_UNSIGNED, 10},
  {"SUNSHINE_CLIENT_HEIGHT", VIBEPOLLO_STREAM_UNSIGNED, 10},
  {"SUNSHINE_CLIENT_FPS", VIBEPOLLO_STREAM_DECIMAL, 17},
  {"SUNSHINE_CLIENT_HDR", VIBEPOLLO_STREAM_BOOLEAN, 5},
  {"SUNSHINE_CLIENT_GCMAP", VIBEPOLLO_STREAM_SIGNED, 11},
  {"SUNSHINE_CLIENT_HOST_AUDIO", VIBEPOLLO_STREAM_BOOLEAN, 5},
  {"SUNSHINE_CLIENT_ENABLE_SOPS", VIBEPOLLO_STREAM_BOOLEAN, 5},
  {"SUNSHINE_CLIENT_AUDIO_CONFIGURATION", VIBEPOLLO_STREAM_AUDIO_LAYOUT, 3},
  {"SUNSHINE_CLIENT_AUDIO_SURROUND_PARAMS", VIBEPOLLO_STREAM_TEXT, 128},
  {"APOLLO_APP_ID", VIBEPOLLO_STREAM_UNSIGNED, 10},
  {"APOLLO_APP_NAME", VIBEPOLLO_STREAM_TEXT, 4096},
  {"APOLLO_APP_UUID", VIBEPOLLO_STREAM_TEXT, 128},
  {"APOLLO_APP_STATUS", VIBEPOLLO_STREAM_APP_STATUS, 11},
  {"APOLLO_CLIENT_UUID", VIBEPOLLO_STREAM_TEXT, 128},
  {"APOLLO_CLIENT_NAME", VIBEPOLLO_STREAM_TEXT, 1024},
  {"APOLLO_CLIENT_WIDTH", VIBEPOLLO_STREAM_UNSIGNED, 10},
  {"APOLLO_CLIENT_HEIGHT", VIBEPOLLO_STREAM_UNSIGNED, 10},
  {"APOLLO_CLIENT_RENDER_WIDTH", VIBEPOLLO_STREAM_UNSIGNED, 10},
  {"APOLLO_CLIENT_RENDER_HEIGHT", VIBEPOLLO_STREAM_UNSIGNED, 10},
  {"APOLLO_CLIENT_SCALE_FACTOR", VIBEPOLLO_STREAM_UNSIGNED, 10},
  {"APOLLO_CLIENT_FPS", VIBEPOLLO_STREAM_UNSIGNED, 10},
  {"APOLLO_CLIENT_HDR", VIBEPOLLO_STREAM_BOOLEAN, 5},
  {"APOLLO_CLIENT_GCMAP", VIBEPOLLO_STREAM_SIGNED, 11},
  {"APOLLO_CLIENT_HOST_AUDIO", VIBEPOLLO_STREAM_BOOLEAN, 5},
  {"APOLLO_CLIENT_ENABLE_SOPS", VIBEPOLLO_STREAM_BOOLEAN, 5},
  {"APOLLO_CLIENT_AUDIO_CONFIGURATION", VIBEPOLLO_STREAM_AUDIO_LAYOUT, 3},
  {"APOLLO_CLIENT_AUDIO_SURROUND_PARAMS", VIBEPOLLO_STREAM_TEXT, 128},
  {"PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE", VIBEPOLLO_STREAM_TOGGLE, 1},
  {"PROTON_SONY_WINDOWS_DEVICE_NAMES", VIBEPOLLO_STREAM_TOGGLE, 1}
};

#define VIBEPOLLO_STREAM_ENVIRONMENT_FIELD_COUNT \
  (sizeof(vibepollo_stream_environment_fields) / sizeof(vibepollo_stream_environment_fields[0]))

static inline bool vibepollo_stream_unsigned_is_safe(const char *value, uint64_t maximum) {
  if (!value || !value[0]) return false;
  uint64_t parsed = 0;
  for (const unsigned char *cursor = (const unsigned char *) value; *cursor; ++cursor) {
    if (*cursor < '0' || *cursor > '9') return false;
    const unsigned int digit = *cursor - '0';
    if (parsed > maximum / 10 ||
        (parsed == maximum / 10 && digit > maximum % 10)) return false;
    parsed = parsed * 10 + digit;
  }
  return true;
}

static inline bool vibepollo_stream_decimal_is_safe(const char *value) {
  // process.cpp uses either %.3f or std::to_string(round(float)), whose
  // compatibility representation has six fractional digits (60.000000).
  uint64_t integer = 0;
  size_t integer_digits = 0, fractional_digits = 0;
  bool positive = false;
  const unsigned char *cursor = (const unsigned char *) value;
  while (*cursor >= '0' && *cursor <= '9') {
    integer = integer * 10 + *cursor++ - '0';
    if (integer > UINT32_MAX || ++integer_digits > 10) return false;
  }
  positive = integer > 0;
  if (*cursor == '.') {
    ++cursor;
    while (*cursor >= '0' && *cursor <= '9') {
      positive = positive || *cursor != '0';
      ++cursor;
      if (++fractional_digits > 6) return false;
    }
    if (!fractional_digits) return false;
  }
  return integer_digits && !*cursor && positive;
}

static inline bool vibepollo_stream_environment_value_is_safe(
    const struct vibepollo_stream_environment_field *field, const char *value) {
  if (!field || !value) return false;
  size_t length = 0;
  for (const unsigned char *cursor = (const unsigned char *) value; *cursor; ++cursor) {
    if (++length > field->maximum_length || *cursor < 0x20 || *cursor == 0x7f) return false;
  }
  switch (field->type) {
    case VIBEPOLLO_STREAM_TEXT:
      // App/client names, UUID fallbacks, and surroundParams are exported as
      // text by process.cpp. Empty values and printable UTF-8 remain literal.
      return true;
    case VIBEPOLLO_STREAM_UNSIGNED:
      return vibepollo_stream_unsigned_is_safe(value, UINT32_MAX);
    case VIBEPOLLO_STREAM_SIGNED:
      return value[0] == '-'
        ? vibepollo_stream_unsigned_is_safe(value + 1, UINT64_C(2147483648))
        : vibepollo_stream_unsigned_is_safe(value, INT32_MAX);
    case VIBEPOLLO_STREAM_DECIMAL:
      return vibepollo_stream_decimal_is_safe(value);
    case VIBEPOLLO_STREAM_BOOLEAN:
      return !strcmp(value, "true") || !strcmp(value, "false");
    case VIBEPOLLO_STREAM_AUDIO_LAYOUT:
      return !strcmp(value, "2.0") || !strcmp(value, "5.1") || !strcmp(value, "7.1");
    case VIBEPOLLO_STREAM_APP_STATUS:
      return !strcmp(value, "STARTING") || !strcmp(value, "RUNNING") ||
             !strcmp(value, "RESUMING") || !strcmp(value, "PAUSING") ||
             !strcmp(value, "TERMINATING");
    case VIBEPOLLO_STREAM_TOGGLE:
      return !strcmp(value, "0") || !strcmp(value, "1");
  }
  return false;
}

static inline bool vibepollo_stream_environment_entry_is_safe(const char *entry, size_t *field_index) {
  if (!entry) return false;
  const char *separator = strchr(entry, '=');
  if (!separator) return false;
  const size_t name_length = (size_t) (separator - entry);
  for (size_t index = 0; index < VIBEPOLLO_STREAM_ENVIRONMENT_FIELD_COUNT; ++index) {
    const struct vibepollo_stream_environment_field *field = &vibepollo_stream_environment_fields[index];
    if (strlen(field->name) == name_length && !memcmp(entry, field->name, name_length)) {
      if (!vibepollo_stream_environment_value_is_safe(field, separator + 1)) return false;
      if (field_index) *field_index = index;
      return true;
    }
  }
  return false;
}

static inline bool vibepollo_stream_environment_is_safe(size_t count, char *const entries[]) {
  if (count > VIBEPOLLO_STREAM_ENVIRONMENT_MAX_ENTRIES || (count && !entries)) return false;
  bool seen[VIBEPOLLO_STREAM_ENVIRONMENT_FIELD_COUNT] = {false};
  size_t bytes = 0;
  for (size_t index = 0; index < count; ++index) {
    size_t field_index = 0;
    if (!vibepollo_stream_environment_entry_is_safe(entries[index], &field_index) ||
        seen[field_index]) return false;
    seen[field_index] = true;
    const size_t length = strlen(entries[index]) + 1;
    if (length > VIBEPOLLO_STREAM_ENVIRONMENT_MAX_BYTES - bytes) return false;
    bytes += length;
  }
  return true;
}

#endif
