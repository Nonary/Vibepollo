#include "../../../../packaging/linux/vibepollo-touch-output.h"

#include <glib.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-server.h>

struct compositor {
  struct wl_display *display;
  struct wl_global *output;
  int control;
};

static void release_output(struct wl_client *client, struct wl_resource *resource) {
  (void) client;
  wl_resource_destroy(resource);
}

static void bind_output(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
  const char *name = data;
  struct wl_resource *resource = wl_resource_create(client, &wl_output_interface, version, id);
  static const struct wl_output_interface implementation = {.release = release_output};
  wl_resource_set_implementation(resource, &implementation, NULL, NULL);
  wl_output_send_geometry(resource, 0, 0, 100, 100, WL_OUTPUT_SUBPIXEL_UNKNOWN, "test", "test", WL_OUTPUT_TRANSFORM_NORMAL);
  wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT, 2560, 1536, 120000);
  wl_output_send_scale(resource, 1);
  wl_output_send_name(resource, name);
  wl_output_send_description(resource, "Private test output");
  wl_output_send_done(resource);
}

static int control_event(int fd, uint32_t mask, void *data) {
  (void) mask;
  struct compositor *compositor = data;
  char command;
  g_assert(read(fd, &command, 1) == 1);
  if (command == 'r') {
    wl_global_destroy(compositor->output);
    compositor->output = wl_global_create(compositor->display, &wl_output_interface, 4, "Virtual-1", bind_output);
    wl_display_flush_clients(compositor->display);
    g_assert(write(fd, "a", 1) == 1);
  } else {
    wl_display_terminate(compositor->display);
  }
  return 0;
}

static void *serve(void *data) {
  wl_display_run(((struct compositor *) data)->display);
  return NULL;
}

static void check_output(const char *requested, bool duplicate, bool expected) {
  int connection[2], control[2];
  g_assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, connection));
  g_assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control));
  struct compositor compositor = {.display = wl_display_create(), .control = control[1]};
  g_assert(compositor.display);
  compositor.output = wl_global_create(compositor.display, &wl_output_interface, 4, "Virtual-1", bind_output);
  if (duplicate) g_assert(wl_global_create(compositor.display, &wl_output_interface, 4, "Virtual-1", bind_output));
  g_assert(wl_client_create(compositor.display, connection[1]));
  struct wl_event_source *source = wl_event_loop_add_fd(wl_display_get_event_loop(compositor.display), control[1], WL_EVENT_READABLE, control_event, &compositor);
  pthread_t thread;
  g_assert(!pthread_create(&thread, NULL, serve, &compositor));
  struct touch_output_probe probe = {.display = wl_display_connect_to_fd(connection[0])};
  g_assert(touch_output_prepare(&probe, requested) == expected);
  if (expected) {
    g_assert(touch_output_available(&probe));
    // Reusing the name does not extend the old wl_output object's lifetime.
    g_assert(write(control[0], "r", 1) == 1);
    char acknowledged;
    g_assert(read(control[0], &acknowledged, 1) == 1);
    g_assert(!touch_output_available(&probe));
  }
  touch_output_close(&probe);
  g_assert(write(control[0], "q", 1) == 1);
  g_assert(!pthread_join(thread, NULL));
  wl_event_source_remove(source);
  wl_display_destroy_clients(compositor.display);
  wl_display_destroy(compositor.display);
  close(control[0]);
  close(control[1]);
}

int main(void) {
  check_output("Virtual-1", false, true);
  check_output("Missing-1", false, false);
  check_output("Virtual-1", true, false);
  struct touch_output_probe disconnected = {0};
  g_assert(!touch_output_prepare(&disconnected, "Virtual-1"));
  touch_output_close(&disconnected);
  return 0;
}
