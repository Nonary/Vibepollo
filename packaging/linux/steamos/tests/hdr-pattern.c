// SPDX-License-Identifier: MIT
// Deterministic HDR10 source using the same Gamescope swapchain color contract
// as Vulkan WSI. Run only inside the isolated test compositor.
#define _GNU_SOURCE
#include "gamescope-swapchain-client-protocol.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct gamescope_swapchain_factory_v2 *factory;
static bool released[2] = {true, true};
static bool rgb10_supported;

static void shm_format(void *data, struct wl_shm *object, uint32_t format) {
  (void) data;
  (void) object;
  if (format == WL_SHM_FORMAT_XBGR2101010) {
    rgb10_supported = true;
  }
}
static const struct wl_shm_listener shm_listener = {shm_format};

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
  (void) data;
  (void) version;
  if (!strcmp(interface, "wl_compositor")) {
    compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
  } else if (!strcmp(interface, "wl_shm")) {
    shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    wl_shm_add_listener(shm, &shm_listener, NULL);
  } else if (!strcmp(interface, "gamescope_swapchain_factory_v2")) {
    factory = wl_registry_bind(registry, name, &gamescope_swapchain_factory_v2_interface, 1);
  }
}

static void removed(void *data, struct wl_registry *registry, uint32_t name) {
  (void) data;
  (void) registry;
  (void) name;
}
static const struct wl_registry_listener registry_listener = {global, removed};

static void release(void *data, struct wl_buffer *buffer) {
  (void) buffer;
  *(bool *) data = true;
}
static const struct wl_buffer_listener buffer_listener = {release};

static uint32_t pq(double nits) {
  const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
  const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
  double p = pow(nits / 10000.0, m1);
  return (uint32_t) lround(1023.0 * pow((c1 + c2 * p) / (1.0 + c3 * p), m2));
}

int main(int argc, char **argv) {
  const bool sdr = argc > 1 && !strcmp(argv[1], "--sdr");
  struct wl_display *display = wl_display_connect(NULL);
  assert(display);
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  assert(wl_display_roundtrip(display) >= 0);
  assert(wl_display_roundtrip(display) >= 0);
  assert(compositor && shm && factory && rgb10_supported);
  Display *xdisplay = XOpenDisplay(NULL);
  assert(xdisplay);
  Window root = DefaultRootWindow(xdisplay);
  Window window = XCreateSimpleWindow(xdisplay, root, 0, 0, 1280, 800, 0, 0, 0);
  XStoreName(xdisplay, window, "Vibepollo HDR validation pattern");
  XMapWindow(xdisplay, window);
  XSync(xdisplay, False);
  Atom actual_type;
  int actual_format;
  unsigned long count, remaining;
  unsigned char *value = NULL;
  assert(XGetWindowProperty(xdisplay, root, XInternAtom(xdisplay, "GAMESCOPE_XWAYLAND_SERVER_ID", False), 0, 1, False, XA_CARDINAL, &actual_type, &actual_format, &count, &remaining, &value) == Success);
  assert(count == 1 && actual_format == 32);
  unsigned server = *(unsigned long *) value;
  XFree(value);
  struct wl_surface *surface = wl_compositor_create_surface(compositor);
  struct gamescope_swapchain *swapchain = gamescope_swapchain_factory_v2_create_swapchain(factory, surface);
  gamescope_swapchain_override_window_content(swapchain, server, window);
  gamescope_swapchain_swapchain_feedback(swapchain, 2, VK_FORMAT_A2B10G10R10_UNORM_PACK32, sdr ? VK_COLOR_SPACE_SRGB_NONLINEAR_KHR : VK_COLOR_SPACE_HDR10_ST2084_EXT, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, 1, "Vibepollo HDR validation");
  gamescope_swapchain_set_present_mode(swapchain, VK_PRESENT_MODE_FIFO_KHR);
  const size_t bytes = 1280 * 800 * 4;
  int fd = memfd_create("vibepollo-hdr-pattern", MFD_CLOEXEC);
  assert(fd >= 0);
  assert(ftruncate(fd, bytes * 2) == 0);
  uint32_t *pixels = mmap(NULL, bytes * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  assert(pixels != MAP_FAILED);
  uint32_t level[] = {0, pq(100), pq(203), pq(1000), pq(4000), pq(1000), pq(1000), pq(1000)};
  for (int b = 0; b < 2; ++b) {
    for (int y = 0; y < 800; ++y) {
      for (int x = 0; x < 1280; ++x) {
        int band = x / 160;
        uint32_t n = sdr ? (band == 0 ? 0 : 1023) : level[band];
        uint32_t r = n, g = n, bl = n;
        if (band == 5) {
          g = bl = 0;
        }
        if (band == 6) {
          r = bl = 0;
        }
        if (band == 7) {
          r = g = 0;
        }
        pixels[b * (bytes / 4) + y * 1280 + x] = r | (g << 10) | (bl << 20) | (3u << 30);
      }
    }
  }
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, bytes * 2);
  struct wl_buffer *buffers[2];
  for (int b = 0; b < 2; ++b) {
    buffers[b] = wl_shm_pool_create_buffer(pool, b * bytes, 1280, 800, 1280 * 4, WL_SHM_FORMAT_XBGR2101010);
    wl_buffer_add_listener(buffers[b], &buffer_listener, &released[b]);
  }
  close(fd);
  printf("PATTERN %s levels: %u %u %u %u\n", sdr ? "SDR" : "HDR", level[1], level[2], level[3], level[4]);
  fflush(stdout);
  time_t end = time(NULL) + 300;
  while (time(NULL) < end) {
    for (int b = 0; b < 2; ++b) {
      if (released[b]) {
        released[b] = false;
        wl_surface_attach(surface, buffers[b], 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, 1280, 800);
        wl_surface_commit(surface);
        break;
      }
    }
    if (wl_display_roundtrip(display) < 0) {
      break;
    }
    usleep(33333);
  }
  XDestroyWindow(xdisplay, window);
  XCloseDisplay(xdisplay);
  wl_display_disconnect(display);
  munmap(pixels, bytes * 2);
  return 0;
}
