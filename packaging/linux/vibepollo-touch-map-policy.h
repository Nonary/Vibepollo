#pragma once

#include <stdbool.h>
#include <string.h>

// Keep the broker verb limited to an event node and a connector, never a
// caller-selected D-Bus object, interface or property.
static inline bool touch_map_arguments_are_safe(const char *node, const char *output) {
  if (!node || strncmp(node, "event", 5) || !node[5] || strlen(node) > 16 ||
      !output || !output[0] || strlen(output) > 128) return false;
  for (const char *p = node + 5; *p; ++p) if (*p < '0' || *p > '9') return false;
  for (const char *p = output; *p; ++p) {
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
          (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')) return false;
  }
  return true;
}
