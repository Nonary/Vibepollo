// Offscreen VKMS buffers exercise the same exporter as an unbridged KWin scanout.
#include "src/platform/linux/dmabuf_egl.h"
#include "src/platform/linux/gpu_snapshot.h"
#include "src/utility.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <glad/gl.h>
#include <linux/dma-buf.h>
#include <sys/mman.h>
#include <thread>
#include <vector>
#include <xf86drm.h>

static GladGLContext test_gl {};

static int find_drm(const char *prefix, int start, const char *driver) {
  for (int i = start; i < start + 12; ++i) {
    char path[64];
    std::snprintf(path, sizeof path, "/dev/dri/%s%d", prefix, i);
    const int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    auto *version = drmGetVersion(fd);
    const bool match = version && !std::strcmp(version->name, driver);
    if (version) {
      drmFreeVersion(version);
    }
    if (match) {
      return fd;
    }
    close(fd);
  }
  return -1;
}

static uint32_t color(uint32_t x, uint32_t y, bool ten_bit) {
  if (ten_bit) {
    return 0xc0000000u | ((x > 17 ? 621u : 251u) << 20) | ((y > 26 ? 201u : 791u) << 10) | 391u;
  }
  return 0xff000000u | ((x > 17 ? 0xacu : 0x39u) << 16) | ((y > 26 ? 0xb3u : 0x27u) << 8) | 0x56u;
}

static bool test(EGLDisplay display, EGLContext context, gbm_device *gpu, int capture, bool ten_bit, egl::gpu_snapshot_t &snapshot, bool reject_first) {
  drm_mode_create_dumb dumb {};
  dumb.width = 217;
  dumb.height = 74;
  dumb.bpp = 32;
  if (drmIoctl(capture, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0) {
    std::perror("CREATE_DUMB");
    return false;
  }
  auto destroy_buffer = util::fail_guard([&] {
    drm_mode_destroy_dumb destroy {dumb.handle};
    drmIoctl(capture, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
  });
  drm_mode_map_dumb map {};
  map.handle = dumb.handle;
  if (drmIoctl(capture, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
    std::perror("MAP_DUMB");
    return false;
  }
  egl::owned_surface_t original;
  auto &src = original.sd;
  if (drmPrimeHandleToFD(capture, dumb.handle, DRM_CLOEXEC | DRM_RDWR, &src.fds[0]) < 0) {
    std::perror("prime export");
    return false;
  }
  src.width = dumb.width;
  src.height = dumb.height;
  src.fourcc = ten_bit ? DRM_FORMAT_ARGB2101010 : DRM_FORMAT_ARGB8888;
  src.pitches[0] = dumb.pitch;
  src.modifier = DRM_FORMAT_MOD_LINEAR;
  src.direct_import_required = true;
  auto bind_image = reinterpret_cast<void (*)(GLenum, void *)>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  GLuint test_texture = 0;
  test_gl.GenTextures(1, &test_texture);
  test_gl.BindTexture(GL_TEXTURE_2D, test_texture);
  auto direct = egl::create_dmabuf_image(display, src);
  if (direct) {
    bind_image(GL_TEXTURE_2D, direct);
  }
  std::printf("%d-bit direct EGL image=%d GL bind error=%#x\n", ten_bit ? 10 : 8, direct != EGL_NO_IMAGE, test_gl.GetError());
  if (direct) {
    eglDestroyImage(display, direct);
  }
  test_gl.DeleteTextures(1, &test_texture);

  GLuint saved[2] {};
  test_gl.GenBuffers(2, saved);
  auto release_saved = util::fail_guard([&] { test_gl.DeleteBuffers(2, saved); });
  for (unsigned generation = 0; generation < 2; ++generation) {
    const uint32_t change = generation ? 0x00151d03 : 0;
    dma_buf_sync sync {DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};
    if (drmIoctl(src.fds[0], DMA_BUF_IOCTL_SYNC, &sync) < 0) {
      std::perror("sync start");
      return false;
    }
    auto *pixels = static_cast<uint8_t *>(mmap(nullptr, dumb.size, PROT_WRITE | PROT_READ, MAP_SHARED, capture, map.offset));
    if (pixels == MAP_FAILED) {
      std::perror("mmap dumb");
      return false;
    }
    for (uint32_t y = 0; y < dumb.height; ++y) {
      auto *row = reinterpret_cast<uint32_t *>(pixels + y * dumb.pitch);
      for (uint32_t x = 0; x < dumb.width; ++x) {
        row[x] = color(x, y, ten_bit) ^ change;
      }
    }
    munmap(pixels, dumb.size);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    if (drmIoctl(src.fds[0], DMA_BUF_IOCTL_SYNC, &sync) < 0) {
      std::perror("sync end");
      return false;
    }
    unsigned attempts = 0;
    EGLImage image = EGL_NO_IMAGE;
    const bool accepted = snapshot.copy(display, reinterpret_cast<gbm::device *>(gpu), src, [&](const auto &output) {
      if (eglGetCurrentContext() != context || eglQueryAPI() != EGL_OPENGL_API) {
        return false;
      }
      if (++attempts == 1 && reject_first && generation == 0) {
        return false;
      }
      image = egl::create_dmabuf_image(display, output);
      if (!image) {
        return false;
      }
      test_gl.GenTextures(1, &test_texture);
      test_gl.BindTexture(GL_TEXTURE_2D, test_texture);
      bind_image(GL_TEXTURE_2D, image);
      if (test_gl.GetError() == GL_NO_ERROR) {
        return true;
      }
      test_gl.DeleteTextures(1, &test_texture);
      eglDestroyImage(display, image);
      return false;
    });
    if (!accepted || (reject_first && generation == 0 ? attempts < 2 : attempts != 1)) {
      return false;
    }
    // Preserve each snapshot before the next copy reuses its output.
    GLuint framebuffer;
    test_gl.GenFramebuffers(1, &framebuffer);
    test_gl.BindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    test_gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, test_texture, 0);
    test_gl.BindBuffer(GL_PIXEL_PACK_BUFFER, saved[generation]);
    test_gl.BufferData(GL_PIXEL_PACK_BUFFER, dumb.width * dumb.height * sizeof(uint32_t), nullptr, GL_STREAM_READ);
    test_gl.ReadPixels(0, 0, dumb.width, dumb.height, GL_BGRA, ten_bit ? GL_UNSIGNED_INT_2_10_10_10_REV : GL_UNSIGNED_BYTE, nullptr);
    test_gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    test_gl.Finish();
    test_gl.DeleteFramebuffers(1, &framebuffer);
    test_gl.DeleteTextures(1, &test_texture);
    eglDestroyImage(display, image);
    if (test_gl.GetError() != GL_NO_ERROR) return false;
  }
  for (unsigned generation = 0; generation < 2; ++generation) {
    const uint32_t change = generation ? 0x00151d03 : 0;
    test_gl.BindBuffer(GL_PIXEL_PACK_BUFFER, saved[generation]);
    std::vector<uint32_t> result(dumb.width * dumb.height);
    test_gl.GetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, result.size() * sizeof(uint32_t), result.data());
    test_gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    const auto gl_error = test_gl.GetError();
    const uint32_t mask = ten_bit ? 0x3fffffffu : 0x00ffffffu;
    if (gl_error != GL_NO_ERROR) {
      std::fprintf(stderr, "snapshot GL error %#x\n", gl_error);
      return false;
    }
    for (uint32_t y = 0; y < dumb.height; ++y) {
      for (uint32_t x = 0; x < dumb.width; ++x) {
        if ((result[y * dumb.width + x] & mask) != ((color(x, y, ten_bit) ^ change) & mask)) {
          std::fprintf(stderr, "pixel check failed at %u,%u observed=%#x expected=%#x\n", x, y, result[y * dumb.width + x], color(x, y, ten_bit) ^ change);
          return false;
        }
      }
    }
    std::printf("%d-bit GPU snapshot preserved all %zu RGB pixels\n", ten_bit ? 10 : 8, result.size());
  }
  return true;
}

int main() {
  const int render = find_drm("renderD", 128, "nvidia-drm");
  const int capture = find_drm("card", 0, "vibeshine_drm");
  if (render < 0 || capture < 0) {
    std::fprintf(stderr, "missing renderer or VKMS card: NVIDIA fd=%d VKMS fd=%d\n", render, capture);
    return 77;
  }
  if (!gladLoaderLoadEGL(EGL_NO_DISPLAY)) {
    return 2;
  }
  auto *gpu = gbm_create_device(render);
  auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
  if (!gpu || !get_display) {
    return 3;
  }
  auto display = get_display(EGL_PLATFORM_GBM_MESA, gpu, nullptr);
  if (!display || !eglInitialize(display, nullptr, nullptr) || !gladLoaderLoadEGL(display) || !eglBindAPI(EGL_OPENGL_API)) {
    return 3;
  }
  constexpr EGLint attrs[] {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
  EGLConfig config;
  EGLint count;
  if (!eglChooseConfig(display, attrs, &config, 1, &count) || !count) {
    return 4;
  }
  auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
  if (!context || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) || !gladLoadGLContext(&test_gl, eglGetProcAddress)) {
    return 5;
  }
  std::printf("Renderer: %s\n", test_gl.GetString(GL_RENDERER));
  bool okay;
  {
    egl::gpu_snapshot_t snapshot;
    const auto choose = glad_eglChooseConfig;
    glad_eglChooseConfig = [](EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) -> EGLBoolean {
      return EGL_FALSE;
    };
    okay = !test(display, context, gpu, capture, false, snapshot, false);
    glad_eglChooseConfig = choose;
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    okay = okay && test(display, context, gpu, capture, false, snapshot, true) &&
           test(display, context, gpu, capture, false, snapshot, false) &&
           test(display, context, gpu, capture, true, snapshot, false);
  }
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(display, context);
  eglTerminate(display);
  gbm_device_destroy(gpu);
  close(render);
  close(capture);
  return okay ? 0 : 1;
}
