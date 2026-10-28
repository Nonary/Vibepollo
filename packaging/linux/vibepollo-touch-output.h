#pragma once

#include <stdbool.h>
#include <string.h>
#include <wayland-client.h>

// wl_output globals represent the compositor's enabled outputs. Keep this
// read-only snapshot alive through the D-Bus assignment, so removal/recreation
// of a connector cannot masquerade as the same output within one attempt.
struct touch_output_entry {
  struct wl_output *output;
  uint32_t global;
  bool present;
  char name[129];
};

struct touch_output_probe {
  struct wl_display *display;
  struct wl_registry *registry;
  struct touch_output_entry entries[64];
  unsigned count;
  bool overflow;
  struct touch_output_entry *target;
};

static void touch_output_name(void *data, struct wl_output *output, const char *name) {
  (void) output;
  struct touch_output_entry *entry = data;
  if (strlen(name) < sizeof(entry->name)) strcpy(entry->name, name);
}

static void touch_output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
                                  int32_t width, int32_t height, int32_t subpixel,
                                  const char *make, const char *model, int32_t transform) {
  (void) data; (void) output; (void) x; (void) y; (void) width; (void) height;
  (void) subpixel; (void) make; (void) model; (void) transform;
}
static void touch_output_mode(void *data, struct wl_output *output, uint32_t flags,
                              int32_t width, int32_t height, int32_t refresh) {
  (void) data; (void) output; (void) flags; (void) width; (void) height; (void) refresh;
}
static void touch_output_done(void *data, struct wl_output *output) { (void) data; (void) output; }
static void touch_output_scale(void *data, struct wl_output *output, int32_t scale) { (void) data; (void) output; (void) scale; }
static void touch_output_description(void *data, struct wl_output *output, const char *description) { (void) data; (void) output; (void) description; }

static void touch_output_global(void *data, struct wl_registry *registry, uint32_t name,
                                const char *interface, uint32_t version) {
  struct touch_output_probe *probe = data;
  if (strcmp(interface, "wl_output") || version < 4) return;
  if (probe->count == 64) { probe->overflow = true; return; }
  struct touch_output_entry *entry = &probe->entries[probe->count++];
  entry->global = name;
  entry->present = true;
  entry->output = wl_registry_bind(registry, name, &wl_output_interface, 4);
  static const struct wl_output_listener listener = {
    .geometry = touch_output_geometry, .mode = touch_output_mode,
    .done = touch_output_done, .scale = touch_output_scale,
    .name = touch_output_name, .description = touch_output_description,
  };
  wl_output_add_listener(entry->output, &listener, entry);
}

static void touch_output_removed(void *data, struct wl_registry *registry, uint32_t name) {
  (void) registry;
  struct touch_output_probe *probe = data;
  for (unsigned i = 0; i < probe->count; ++i) {
    if (probe->entries[i].global == name) probe->entries[i].present = false;
  }
}

static bool touch_output_prepare(struct touch_output_probe *probe, const char *name) {
  if (!probe->display) return false;
  probe->registry = wl_display_get_registry(probe->display);
  static const struct wl_registry_listener listener = {
    .global = touch_output_global, .global_remove = touch_output_removed,
  };
  wl_registry_add_listener(probe->registry, &listener, probe);
  if (wl_display_roundtrip(probe->display) < 0 || wl_display_roundtrip(probe->display) < 0) return false;
  unsigned matches = 0;
  for (unsigned i = 0; i < probe->count; ++i) {
    if (probe->entries[i].present && !strcmp(probe->entries[i].name, name)) {
      probe->target = &probe->entries[i];
      ++matches;
    }
  }
  return matches == 1 && !probe->overflow;
}

static bool touch_output_available(struct touch_output_probe *probe) {
  return probe->target && wl_display_roundtrip(probe->display) >= 0 && !probe->overflow && probe->target->present;
}

static void touch_output_close(struct touch_output_probe *probe) {
  for (unsigned i = 0; i < probe->count; ++i) wl_output_destroy(probe->entries[i].output);
  if (probe->registry) wl_registry_destroy(probe->registry);
  if (probe->display) wl_display_disconnect(probe->display);
}
