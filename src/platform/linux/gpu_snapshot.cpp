/**
 * @file src/platform/linux/gpu_snapshot.cpp
 * @brief GPU fallback for DMA-BUFs that cannot bind as ordinary RGB textures.
 */
#include "gpu_snapshot.h"
#include "dmabuf_egl.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <dlfcn.h>
#include <fcntl.h>
#include <gbm.h>
#include <glad/egl.h>
#include <glad/gles2.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <optional>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#if defined(__x86_64__) || defined(__i386__)
  #include <cpuid.h>
  #include <immintrin.h>
#endif

namespace egl {
  namespace {
    // GBM is already optional at runtime for capture backends that don't use DRM.
    struct gbm_api_t {
      void *library = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
      decltype(&gbm_bo_create) create = load<decltype(create)>("gbm_bo_create");
      decltype(&gbm_bo_destroy) destroy = load<decltype(destroy)>("gbm_bo_destroy");
      decltype(&gbm_bo_get_fd) fd = load<decltype(fd)>("gbm_bo_get_fd");
      decltype(&gbm_bo_get_stride) stride = load<decltype(stride)>("gbm_bo_get_stride");
      decltype(&gbm_bo_get_offset) offset = load<decltype(offset)>("gbm_bo_get_offset");
      decltype(&gbm_bo_get_modifier) modifier = load<decltype(modifier)>("gbm_bo_get_modifier");
      decltype(&gbm_bo_get_plane_count) planes = load<decltype(planes)>("gbm_bo_get_plane_count");

      template<class T>
      T load(const char *symbol) const {
        return library ? reinterpret_cast<T>(dlsym(library, symbol)) : nullptr;
      }

      explicit operator bool() const {
        return create && destroy && fd && stride && offset && modifier && planes;
      }
    };

    const gbm_api_t &gbm_api() {
      static const gbm_api_t api;
      return api;
    }

    // Save both draw/read surfaces as CUDA/GL and VAAPI can own current
    // desktop GL contexts on this thread. GLES has a separate dispatch table.
    struct current_context_t {
      EGLDisplay temporary;
      EGLenum api = eglQueryAPI();
      EGLDisplay display = eglGetCurrentDisplay();
      EGLSurface draw = eglGetCurrentSurface(EGL_DRAW);
      EGLSurface read = eglGetCurrentSurface(EGL_READ);
      EGLContext context = eglGetCurrentContext();
      bool restore_pending = false;

      ~current_context_t() {
        if (restore_pending) {
          restore();
        }
      }

      bool bind_gles() {
        restore_pending = eglBindAPI(EGL_OPENGL_ES_API);
        return restore_pending;
      }

      bool restore() {
        restore_pending = false;
        const bool released = eglMakeCurrent(temporary, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (!eglBindAPI(api)) {
          return false;
        }
        return context == EGL_NO_CONTEXT ? released : eglMakeCurrent(display, draw, read, context);
      }
    };

    bool is_native_drm_export(int fd) {
      char path[64];
      std::snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
      const auto close_info = [](FILE *file) { std::fclose(file); };
      std::unique_ptr<FILE, decltype(close_info)> info(std::fopen(path, "re"), close_info);
      if (!info) {
        return true;
      }
      char line[128];
      while (std::fgets(line, sizeof(line), info.get())) {
        if (const std::string_view field {line}; field.starts_with("exp_name:")) {
          const auto value = field.find_first_not_of(" \t", 9);
          return value == std::string_view::npos || field.substr(value) == "drm\n";
        }
      }
      return true;
    }

#if defined(__x86_64__) || defined(__i386__)
    __attribute__((target("clflushopt"))) void flush_cache_lines(const char *data, std::size_t size, unsigned stride) {
      for (std::size_t offset = 0; offset < size; offset += stride) {
        _mm_clflushopt(const_cast<char *>(data + offset));
      }
      _mm_sfence();
    }
#endif

    // A pinned descriptor keeps the identity valid even if the producer closes
    // its FD and exports the same scanout again under a different FD number.
    struct input_key_t {
      dev_t device;
      ino_t inode;
      int width, height;
      std::uint32_t fourcc, pitch, offset;
      std::uint64_t modifier;

      bool operator==(const input_key_t &) const = default;
    };

    struct input_t {
      EGLDisplay display;
      EGLImage image = EGL_NO_IMAGE;
      std::optional<input_key_t> key;
      int pinned_fd = -1;
      void *mapped = MAP_FAILED;
      std::size_t extent = 0;

      explicit input_t(EGLDisplay display): display(display) {}
      input_t(const input_t &) = delete;
      input_t &operator=(const input_t &) = delete;
      ~input_t() {
        if (image != EGL_NO_IMAGE) eglDestroyImage(display, image);
        if (mapped != MAP_FAILED) munmap(mapped, extent);
        if (pinned_fd >= 0) close(pinned_fd);
      }

      // Native VKMS shmem can contain CPU-written lines that the NVIDIA import
      // cannot snoop. Every presentation still needs writeback, even on a hit.
      bool sync(const surface_descriptor_t &source) {
        if (!source.direct_import_required || source.modifier != 0 || source.fds[1] >= 0 ||
            source.fds[2] >= 0 || source.fds[3] >= 0 || !is_native_drm_export(source.fds[0])) {
          return true;
        }
#if defined(__x86_64__) || defined(__i386__)
        unsigned eax, ebx, ecx, edx;
        if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) || !(edx & (1U << 19))) {
          return false;
        }
        const unsigned stride = ((ebx >> 8) & 0xffU) * 8;
        const std::uint64_t row_bytes = std::uint64_t(source.width) * 4;
        const std::uint64_t needed = std::uint64_t(source.offsets[0]) +
          std::uint64_t(source.height - 1) * source.pitches[0] + row_bytes;
        if (!stride || row_bytes > source.pitches[0] || needed > SIZE_MAX) {
          return false;
        }
        const auto fd = source.fds[0];
        if (mapped == MAP_FAILED) {
          const auto size = lseek(fd, 0, SEEK_END);
          (void) lseek(fd, 0, SEEK_SET);
          if (size <= 0 || needed > static_cast<std::uint64_t>(size)) return false;
          mapped = mmap(nullptr, needed, PROT_READ, MAP_SHARED, fd, 0);
          if (mapped == MAP_FAILED) return false;
          extent = needed;
        }
        dma_buf_sync sync {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
        if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
          return false;
        }
        const auto first_line = source.offsets[0] - source.offsets[0] % stride;
        const auto *begin = static_cast<const char *>(mapped) + first_line;
        const auto length = static_cast<std::size_t>(extent) - first_line;
        if (__builtin_cpu_supports("clflushopt")) {
          flush_cache_lines(begin, length, stride);
        } else {
          for (std::size_t offset = 0; offset < length; offset += stride) {
            _mm_clflush(begin + offset);
          }
          _mm_mfence();
        }
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
        return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0;
#else
        return false;
#endif
      }
    };

    constexpr std::array<std::uint32_t, 4> rgb8 {
      GBM_FORMAT_XRGB8888, GBM_FORMAT_ARGB8888, GBM_FORMAT_XBGR8888, GBM_FORMAT_ABGR8888
    };
    constexpr std::array<std::uint32_t, 4> rgb10 {
      GBM_FORMAT_XRGB2101010, GBM_FORMAT_ARGB2101010, GBM_FORMAT_XBGR2101010, GBM_FORMAT_ABGR2101010
    };
    constexpr std::array<std::uint32_t, 3> usage {
      GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT, GBM_BO_USE_RENDERING
    };

    int precision(std::uint32_t fourcc) {
      if (std::ranges::find(rgb10, fourcc) != rgb10.end()) {
        return 10;
      }
      if (std::ranges::find(rgb8, fourcc) != rgb8.end() ||
          fourcc == GBM_FORMAT_RGBA8888 || fourcc == GBM_FORMAT_BGRA8888) {
        return 8;
      }
      return 0;
    }
  }

  struct gpu_snapshot_t::impl_t {
    EGLDisplay display;
    gbm_device *device;
    bool nvidia;
    std::array<std::unique_ptr<input_t>, 4> inputs;
    std::size_t next_input = 0;
    GLuint external_texture = 0;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    GladGLES2Context gl {};
    GLuint program = 0;
    GLint position = -1;
    GLint texcoord = -1;
    GLuint framebuffer = 0;
    GLuint texture = 0;
    EGLImage image = EGL_NO_IMAGE;
    gbm_bo *buffer = nullptr;
    owned_surface_t output;
    int output_precision = 0;
    std::size_t layout = 0;
    bool accepted = false;
    std::array<int, 3> rejected_output {};
    std::chrono::steady_clock::time_point retry_layouts_after {};

    impl_t(EGLDisplay disp, gbm::device *gbm):
        display(disp),
        device(reinterpret_cast<gbm_device *>(gbm)),
        nvidia(false) {
      const char *vendor = eglQueryString(display, EGL_VENDOR);
      nvidia = vendor && std::string_view(vendor).contains("NVIDIA");
    }

    ~impl_t() {
      // Destroying the private context releases its GL objects without touching
      // the caller's current context or its texture bindings.
      if (context != EGL_NO_CONTEXT) {
        eglDestroyContext(display, context);
      }
      if (surface != EGL_NO_SURFACE) {
        eglDestroySurface(display, surface);
      }
      if (image != EGL_NO_IMAGE) {
        eglDestroyImage(display, image);
      }
      if (buffer) {
        gbm_api().destroy(buffer);
      }
    }

    bool create_context() {
      // Mesa's GBM platform can expose only window configs. Rendering to our
      // framebuffer needs no EGL surface when surfaceless contexts are available.
      const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
      const bool surfaceless = extensions && std::string_view(extensions).contains("EGL_KHR_surfaceless_context");
      const EGLint requirements[] {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, surfaceless ? 0 : EGL_PBUFFER_BIT, EGL_NONE};
      constexpr EGLint attributes[] {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
      constexpr EGLint pbuffer[] {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
      EGLConfig config {};
      EGLint count = 0;
      if (!eglChooseConfig(display, requirements, &config, 1, &count) || count == 0) {
        return false;
      }
      context = eglCreateContext(display, config, EGL_NO_CONTEXT, attributes);
      if (!surfaceless && context != EGL_NO_CONTEXT) {
        surface = eglCreatePbufferSurface(display, config, pbuffer);
      }
      return context != EGL_NO_CONTEXT && (surfaceless || surface != EGL_NO_SURFACE);
    }

    GLuint compile(GLenum type, const char *text) {
      const auto shader = gl.CreateShader(type);
      gl.ShaderSource(shader, 1, &text, nullptr);
      gl.CompileShader(shader);
      GLint compiled = GL_FALSE;
      gl.GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
      if (!compiled) {
        gl.DeleteShader(shader);
        return 0;
      }
      return shader;
    }

    bool initialize() {
      if (!gladLoadGLES2Context(&gl, eglGetProcAddress) || !gl.OES_EGL_image || !gl.OES_EGL_image_external) {
        return false;
      }
      const GLuint vertex = compile(GL_VERTEX_SHADER,
        "attribute vec2 position; attribute vec2 texcoord; varying vec2 uv;"
        "void main() { gl_Position = vec4(position, 0.0, 1.0); uv = texcoord; }");
      const GLuint fragment = compile(GL_FRAGMENT_SHADER,
        "#extension GL_OES_EGL_image_external : require\n"
        "precision highp float; varying vec2 uv; uniform highp samplerExternalOES source;"
        "void main() { gl_FragColor = texture2D(source, uv); }");
      program = gl.CreateProgram();
      if (vertex && fragment) {
        gl.AttachShader(program, vertex);
        gl.AttachShader(program, fragment);
        gl.LinkProgram(program);
      }
      gl.DeleteShader(vertex);
      gl.DeleteShader(fragment);
      GLint linked = GL_FALSE;
      gl.GetProgramiv(program, GL_LINK_STATUS, &linked);
      if (!linked) {
        return false;
      }
      position = gl.GetAttribLocation(program, "position");
      texcoord = gl.GetAttribLocation(program, "texcoord");
      const auto sampler = gl.GetUniformLocation(program, "source");
      if (position < 0 || texcoord < 0 || sampler < 0) {
        return false;
      }
      gl.UseProgram(program);
      gl.Uniform1i(sampler, 0);
      gl.Disable(GL_DITHER);
      return gl.GetError() == GL_NO_ERROR;
    }

    void reset_output() {
      gl.DeleteFramebuffers(1, &framebuffer);
      gl.DeleteTextures(1, &texture);
      framebuffer = texture = 0;
      if (image != EGL_NO_IMAGE) {
        eglDestroyImage(display, image);
        image = EGL_NO_IMAGE;
      }
      if (buffer) {
        gbm_api().destroy(buffer);
        buffer = nullptr;
      }
      reset_surface(output.sd);
      output_precision = 0;
    }

    bool allocate_output(int width, int height, int bits, std::uint32_t fourcc, std::uint32_t flags) {
      const auto &api = gbm_api();
      buffer = api.create(device, width, height, fourcc, flags);
      if (!buffer || api.planes(buffer) != 1) {
        return false;
      }
      auto &sd = output.sd;
      sd.width = width;
      sd.height = height;
      sd.fourcc = fourcc;
      sd.fds[0] = api.fd(buffer);
      sd.pitches[0] = api.stride(buffer);
      sd.offsets[0] = api.offset(buffer, 0);
      sd.modifier = api.modifier(buffer);
      sd.direct_import_required = true;
      if (sd.fds[0] < 0 || fcntl(sd.fds[0], F_SETFD, FD_CLOEXEC) < 0 || sd.pitches[0] == 0) {
        return false;
      }
      image = create_dmabuf_image(display, sd);
      if (image == EGL_NO_IMAGE) {
        return false;
      }
      gl.GenTextures(1, &texture);
      gl.BindTexture(GL_TEXTURE_2D, texture);
      gl.EGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
      gl.GenFramebuffers(1, &framebuffer);
      gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
      gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
      if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        return false;
      }
      for (auto channel : {GL_RED_BITS, GL_GREEN_BITS, GL_BLUE_BITS}) {
        GLint actual = 0;
        gl.GetIntegerv(channel, &actual);
        if (actual < bits) {
          return false;
        }
      }
      return gl.GetError() == GL_NO_ERROR;
    }

    bool prepare_output(const surface_descriptor_t &source, int bits, std::size_t candidate) {
      if (output_precision == bits && output.sd.width == source.width && output.sd.height == source.height && layout == candidate) {
        return true;
      }
      reset_output();
      const auto &formats = bits == 10 ? rgb10 : rgb8;
      if (!allocate_output(source.width, source.height, bits, formats[candidate % formats.size()], usage[candidate / formats.size()])) {
        reset_output();
        for (int n = 0; n < 8 && gl.GetError() != GL_NO_ERROR; ++n) {}
        return false;
      }
      output_precision = bits;
      layout = candidate;
      return true;
    }

    input_t &source_input(const surface_descriptor_t &source, input_t &temporary) {
      struct stat identity {};
      if (source.fds[1] >= 0 || source.fds[2] >= 0 || source.fds[3] >= 0 ||
          fstat(source.fds[0], &identity) != 0 || !identity.st_ino) {
        return temporary;
      }
      const input_key_t key {identity.st_dev, identity.st_ino, source.width, source.height,
                             source.fourcc, source.pitches[0], source.offsets[0], source.modifier};
      for (const auto &input : inputs) {
        if (input && input->key == key) return *input;
      }
      const int pinned = fcntl(source.fds[0], F_DUPFD_CLOEXEC, 0);
      if (pinned < 0) return temporary;
      auto &entry = inputs[next_input];
      next_input = (next_input + 1) % inputs.size();
      entry = std::make_unique<input_t>(display);
      entry->key = key;
      entry->pinned_fd = pinned;
      return *entry;
    }

    bool draw(const surface_descriptor_t &source, input_t &input) {
      if (input.image == EGL_NO_IMAGE) {
        input.image = create_dmabuf_image(display, source);
        if (input.image == EGL_NO_IMAGE) return false;
      }
      // Rebind on every presentation to sample newly written producer pixels.
      if (!external_texture) gl.GenTextures(1, &external_texture);
      gl.BindTexture(GL_TEXTURE_EXTERNAL_OES, external_texture);
      gl.TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      gl.TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      gl.TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      gl.TexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      gl.EGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, input.image);
      bool success = gl.GetError() == GL_NO_ERROR;
      if (success) {
        constexpr GLfloat positions[] {-1, -1, 1, -1, -1, 1, 1, 1};
        constexpr GLfloat coordinates[] {0, 0, 1, 0, 0, 1, 1, 1};
        gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        gl.Viewport(0, 0, source.width, source.height);
        gl.VertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 0, positions);
        gl.VertexAttribPointer(texcoord, 2, GL_FLOAT, GL_FALSE, 0, coordinates);
        gl.EnableVertexAttribArray(position);
        gl.EnableVertexAttribArray(texcoord);
        gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        // Complete the GPU copy before a different API reads the DMA-BUF.
        // The shader transfers the pixels.
        gl.Finish();
        success = gl.GetError() == GL_NO_ERROR;
      }
      return success;
    }
  };

  gpu_snapshot_t::gpu_snapshot_t() = default;
  gpu_snapshot_t::~gpu_snapshot_t() = default;

  bool gpu_snapshot_t::has_output() const {
    return impl && impl->accepted;
  }

  bool gpu_snapshot_t::copy(void *display, gbm::device *device, const surface_descriptor_t &source, const import_t &import) {
    const int bits = precision(source.fourcc);
    if (!display || !device || !gbm_api() || !bits || source.width <= 0 || source.height <= 0 ||
        source.fds[0] < 0 || std::chrono::steady_clock::now() < retry_after || !wait_for_source(source)) {
      return false;
    }
    current_context_t previous {display};
    if (!previous.bind_gles()) {
      retry_after = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      return false;
    }
    if (!impl) {
      auto initializing = std::make_unique<impl_t>(display, device);
      if (!initializing->create_context() ||
          !eglMakeCurrent(display, initializing->surface, initializing->surface, initializing->context) ||
          !initializing->initialize()) {
        previous.restore();
        retry_after = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        return false;
      }
      impl = std::move(initializing);
    }
    const std::array output_shape {source.width, source.height, bits};
    if (impl->display != display || impl->device != reinterpret_cast<gbm_device *>(device) ||
        (impl->rejected_output == output_shape && std::chrono::steady_clock::now() < impl->retry_layouts_after)) {
      return false;
    }

    input_t temporary(display);
    auto &input = impl->source_input(source, temporary);
    if (impl->nvidia && !input.sync(source)) return false;

    constexpr std::size_t layouts = usage.size() * rgb8.size();
    const auto first = impl->accepted ? impl->layout : 0;
    impl->accepted = false;
    for (std::size_t i = 0; i < layouts; ++i) {
      if (!eglMakeCurrent(display, impl->surface, impl->surface, impl->context)) {
        previous.restore();
        impl.reset();
        retry_after = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        return false;
      }
      if (!impl->prepare_output(source, bits, (first + i) % layouts)) {
        continue;
      }
      if (!impl->draw(source, input) || !previous.restore()) {
        return false;
      }
      if (import(impl->output.sd)) {
        impl->accepted = true;
        impl->rejected_output = {};
        return true;
      }
      if (!previous.bind_gles()) {
        return false;
      }
    }
    // A CPU-only consumer should not pay for a dozen full GPU copies per
    // frame. Retry negotiation periodically, or immediately on a format change.
    impl->rejected_output = output_shape;
    impl->retry_layouts_after = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    return false;
  }
}
