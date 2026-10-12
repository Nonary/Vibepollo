#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <wayland-client.h>

#include "dpms.h"

#define MAX_OUTPUTS 64
#define MAX_OUTPUT_NAME 127

struct observed_output {
  struct observer *observer;
  uint32_t registry_name;
  struct wl_list link;
  struct wl_output *output;
  struct org_kde_kwin_dpms *dpms;
  char *name;
  unsigned mode;
  bool mode_known;
  bool supported;
  bool support_known;
  bool layout_changed;
  int32_t x, y, width, height, refresh, scale, transform;
};

struct observer {
  struct wl_display *display;
  struct wl_registry *registry;
  struct org_kde_kwin_dpms_manager *manager;
  struct wl_list outputs;
  unsigned output_count;
  bool failed;
  bool watch;
  bool ready;
};

static void dpms_supported(void *data, struct org_kde_kwin_dpms *dpms, uint32_t supported) {
  (void) dpms;
  struct observed_output *output = data;
  output->support_known = true;
  output->supported = supported != 0;
}

static void dpms_mode(void *data, struct org_kde_kwin_dpms *dpms, uint32_t mode) {
  (void) dpms;
  struct observed_output *output = data;
  output->mode_known = true;
  output->mode = mode;
}

static void emit_if_known(struct observed_output *output) {
  if (!output->name || !output->support_known || !output->mode_known) return;
  if (printf("%s%s\t%u\t%u\n", output->observer->watch ? "S\t" : "", output->name,
             output->supported ? 1u : 0u, output->mode) < 0 || fflush(stdout)) {
    // A closed consumer is an ordinary cancellation path.
    _exit(0);
  }
}

static void dpms_done(void *data, struct org_kde_kwin_dpms *dpms) {
  (void) dpms;
  emit_if_known(data);
}

static const struct org_kde_kwin_dpms_listener dpms_listener = {
  .supported = dpms_supported,
  .mode = dpms_mode,
  .done = dpms_done,
};

static void output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
                            int32_t physical_width, int32_t physical_height, int32_t subpixel,
                            const char *make, const char *model, int32_t transform) {
  (void) output; (void) physical_width;
  (void) physical_height; (void) subpixel; (void) make; (void) model;
  struct observed_output *observed = data;
  observed->layout_changed |= observed->x != x || observed->y != y || observed->transform != transform;
  observed->x = x; observed->y = y; observed->transform = transform;
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh) {
  (void) output;
  if (!(flags & WL_OUTPUT_MODE_CURRENT)) return;
  struct observed_output *observed = data;
  observed->layout_changed |= observed->width != width || observed->height != height || observed->refresh != refresh;
  observed->width = width; observed->height = height; observed->refresh = refresh;
}

static void output_done(void *data, struct wl_output *output) {
  (void) output;
  struct observed_output *observed = data;
  if (observed->observer->watch && observed->observer->ready && observed->layout_changed && observed->name) {
    if (printf("T\t%s\n", observed->name) < 0 || fflush(stdout)) _exit(0);
  }
  observed->layout_changed = false;
}

static void output_scale(void *data, struct wl_output *output, int32_t factor) {
  (void) output;
  struct observed_output *observed = data;
  observed->layout_changed |= observed->scale != factor;
  observed->scale = factor;
}

static void attach_dpms(struct observer *observer, struct observed_output *output) {
  if (!observer->manager || !output->output || output->dpms) return;
  output->dpms = org_kde_kwin_dpms_manager_get(observer->manager, output->output);
  if (!output->dpms || org_kde_kwin_dpms_add_listener(output->dpms, &dpms_listener, output)) {
    observer->failed = true;
  }
}

static void output_name(void *data, struct wl_output *output, const char *name) {
  (void) output;
  struct observed_output *observed = data;
  if (!name || !*name || strlen(name) > MAX_OUTPUT_NAME ||
      strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") != strlen(name)) return;
  char *copy = strdup(name ? name : "");
  if (!copy) return;
  free(observed->name);
  observed->name = copy;
  observed->layout_changed = true;
  emit_if_known(observed);
}

static void output_description(void *data, struct wl_output *output, const char *description) {
  (void) data; (void) output; (void) description;
}

static const struct wl_output_listener output_listener = {
  .geometry = output_geometry,
  .mode = output_mode,
  .done = output_done,
  .scale = output_scale,
  .name = output_name,
  .description = output_description,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
  struct observer *observer = data;
  if (!strcmp(interface, org_kde_kwin_dpms_manager_interface.name) && !observer->manager) {
    observer->manager = wl_registry_bind(registry, name,
      &org_kde_kwin_dpms_manager_interface,
      version < 1 ? version : 1);
    if (!observer->manager) observer->failed = true;
    struct observed_output *output;
    wl_list_for_each(output, &observer->outputs, link) attach_dpms(observer, output);
    return;
  }
  if (strcmp(interface, wl_output_interface.name) || version < 4) return;
  if (observer->output_count >= MAX_OUTPUTS) {
    observer->failed = true;
    return;
  }
  struct observed_output *output = calloc(1, sizeof(*output));
  if (!output) {
    observer->failed = true;
    return;
  }
  wl_list_init(&output->link);
  output->observer = observer;
  output->registry_name = name;
  ++observer->output_count;
  wl_list_insert(observer->outputs.prev, &output->link);
  output->output = wl_registry_bind(registry, name, &wl_output_interface, 4);
  if (!output->output || wl_output_add_listener(output->output, &output_listener, output)) {
    observer->failed = true;
    return;
  }
  attach_dpms(observer, output);
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
  (void) registry;
  struct observer *observer = data;
  struct observed_output *output, *temporary;
  wl_list_for_each_safe(output, temporary, &observer->outputs, link) {
    if (output->registry_name != name) continue;
    if (observer->watch && observer->ready && output->name) {
      if (printf("T\t%s\n", output->name) < 0 || fflush(stdout)) _exit(0);
    }
    wl_list_remove(&output->link);
    if (output->dpms) org_kde_kwin_dpms_release(output->dpms);
    if (output->output) wl_output_destroy(output->output);
    free(output->name);
    free(output);
    --observer->output_count;
  }
}

static const struct wl_registry_listener registry_listener = {
  .global = registry_global,
  .global_remove = registry_remove,
};

static void destroy_observer(struct observer *observer) {
  struct observed_output *output, *temporary;
  wl_list_for_each_safe(output, temporary, &observer->outputs, link) {
    wl_list_remove(&output->link);
    if (output->dpms) org_kde_kwin_dpms_release(output->dpms);
    if (output->output) wl_output_destroy(output->output);
    free(output->name);
    free(output);
  }
  if (observer->manager) wl_proxy_destroy((struct wl_proxy *) observer->manager);
  if (observer->registry) wl_registry_destroy(observer->registry);
  if (observer->display) wl_display_disconnect(observer->display);
}

int main(int argc, char **argv) {
  const bool watch = argc == 2 && !strcmp(argv[1], "--watch");
  if ((argc != 1 && !watch) || getuid() == 0 || geteuid() != getuid() || getegid() != getgid() ||
      prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
      prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) return 126;
  alarm(4);
  struct observer observer = {.watch = watch};
  wl_list_init(&observer.outputs);
  observer.display = wl_display_connect(NULL);
  if (!observer.display) return 0; // Unsupported/unavailable session: caller treats it as unknown.
  observer.registry = wl_display_get_registry(observer.display);
  if (!observer.registry || wl_registry_add_listener(observer.registry, &registry_listener, &observer) ||
      wl_display_roundtrip(observer.display) < 0 || observer.failed || (!observer.manager && !watch)) {
    destroy_observer(&observer);
    return 0;
  }
  struct observed_output *output;
  wl_list_for_each(output, &observer.outputs, link) attach_dpms(&observer, output);
  if (wl_display_roundtrip(observer.display) < 0 || observer.failed) {
    destroy_observer(&observer);
    return 0;
  }
  alarm(0);
  // One subscription remains idle between compositor events. Its initial
  // state only seeds the host baseline; it is not a monitor wake.
  if (watch) {
    observer.ready = true;
    if (puts("READY") == EOF || fflush(stdout)) _exit(0);
    while (!observer.failed && wl_display_dispatch(observer.display) >= 0) {}
  }
  // DPMS events are observations only. The helper sends no set request.
  destroy_observer(&observer);
  return 0;
}
