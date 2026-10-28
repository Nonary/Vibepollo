#ifndef TEST_SESSION_DISPLAY_REQUEST_H
#define TEST_SESSION_DISPLAY_REQUEST_H

#include <stdarg.h>

// The supported snapshot budget is 64 outputs, with at most seven activation
// properties each. Keep room for one extra property to test rejection.
enum {
  TEST_DISPLAY_MAX_PROPERTIES = 64 * 7,
  TEST_DISPLAY_PROPERTY_BYTES = 513,
};

struct display_request_fixture {
  char properties[TEST_DISPLAY_MAX_PROPERTIES + 1][TEST_DISPLAY_PROPERTY_BYTES];
  char *argv[TEST_DISPLAY_MAX_PROPERTIES + 4];
  int argc;
  size_t message_length;
};

static void initialize_display_request(struct display_request_fixture *fixture) {
  memset(fixture, 0, sizeof(*fixture));
  fixture->argv[0] = (char *) "vibepollo-session-exec";
  fixture->argv[1] = (char *) "display-apply";
  fixture->argc = 2;
  fixture->message_length = sizeof(struct vibepollo_session_message) + sizeof("display-apply");
}

static bool append_display_property(struct display_request_fixture *fixture, const char *format, ...) {
  const int index = fixture->argc - 2;
  if (index < 0 || index > TEST_DISPLAY_MAX_PROPERTIES) return false;
  va_list arguments;
  va_start(arguments, format);
  const int length = vsnprintf(fixture->properties[index], TEST_DISPLAY_PROPERTY_BYTES, format, arguments);
  va_end(arguments);
  if (length < 1 || length >= TEST_DISPLAY_PROPERTY_BYTES) return false;
  fixture->argv[fixture->argc++] = fixture->properties[index];
  fixture->argv[fixture->argc] = NULL;
  fixture->message_length += (size_t) length + 1;
  return true;
}

static bool compose_eight_client_display_request(struct display_request_fixture *fixture) {
  initialize_display_request(fixture);
  // Remote Monitor composition retains the three enabled physical outputs.
  for (int output = 1; output <= 3; ++output) {
    if (!append_display_property(fixture, "output.DP-%d.enable", output) ||
        !append_display_property(fixture, "output.DP-%d.position.%d,0", output, (output - 1) * 3840) ||
        !append_display_property(fixture, "output.DP-%d.priority.%d", output, output)) return false;
  }
  for (int output = 1; output <= 8; ++output) {
    if (!append_display_property(fixture, "output.Virtual-%d.enable", output) ||
        !append_display_property(fixture, "output.Virtual-%d.mode.3840x2160_120000", output) ||
        !append_display_property(fixture, "output.Virtual-%d.vrrpolicy.always", output) ||
        !append_display_property(fixture, "output.Virtual-%d.scale.1.000000", output)) return false;
    // Odd clients request SDR; newly connected HDR clients are rearmed after
    // all base properties, in the same complete activation transaction.
    if (output % 2 && !append_display_property(fixture, "output.Virtual-%d.hdr.disable", output)) return false;
    if (!append_display_property(fixture, "output.Virtual-%d.position.%d,0", output, (output + 2) * 3840) ||
        !append_display_property(fixture, "output.Virtual-%d.priority.%d", output, output + 3)) return false;
  }
  for (int output = 2; output <= 8; output += 2) {
    if (!append_display_property(fixture, "output.Virtual-%d.hdr.disable", output)) return false;
  }
  return fixture->argc == 2 + 8 * 7 + 3 * 3;
}

static bool compose_maximum_restore_request(struct display_request_fixture *fixture) {
  initialize_display_request(fixture);
  char name[129], mode[129];
  memset(mode, 'm', 128);
  mode[128] = 0;
  for (int output = 0; output < 64; ++output) {
    memset(name, 'N', 125);
    if (snprintf(name + 125, 4, "%03d", output) != 3 ||
        !append_display_property(fixture, "output.%s.enable", name) ||
        !append_display_property(fixture, "output.%s.mode.%s", name, mode) ||
        !append_display_property(fixture, "output.%s.rotation.flipped270", name) ||
        !append_display_property(fixture, "output.%s.scale.5.000000", name) ||
        !append_display_property(fixture, "output.%s.position.-1000000,-1000000", name) ||
        !append_display_property(fixture, "output.%s.priority.64", name) ||
        !append_display_property(fixture, "output.%s.hdr.disable", name)) return false;
  }
  return fixture->argc == TEST_DISPLAY_MAX_PROPERTIES + 2;
}

static size_t encode_display_request(const struct display_request_fixture *fixture,
                                    unsigned char *packet, size_t capacity) {
  if (fixture->message_length > capacity) return 0;
  struct vibepollo_session_message header = {
    .magic = VIBEPOLLO_SESSION_PROTOCOL_MAGIC,
    .version = VIBEPOLLO_SESSION_PROTOCOL_VERSION,
    .type = VIBEPOLLO_SESSION_REQUEST,
    .payload_length = (uint32_t) (fixture->message_length - sizeof(header)),
    .argument_count = (uint32_t) (fixture->argc - 1),
    .generation = 42,
  };
  memcpy(packet, &header, sizeof(header));
  size_t offset = sizeof(header);
  for (int index = 1; index < fixture->argc; ++index) {
    const size_t length = strlen(fixture->argv[index]) + 1;
    memcpy(packet + offset, fixture->argv[index], length);
    offset += length;
  }
  return offset;
}

#endif
