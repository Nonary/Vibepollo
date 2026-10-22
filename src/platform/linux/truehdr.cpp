#include "truehdr.h"
#include "truehdr_peak.h"

#include <algorithm>
#include <dlfcn.h>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace platf::linux_truehdr {
  namespace {
    // Published NvCVImage C ABI. No NVIDIA SDK headers or implementation are
    // required to build the host; the pinned runtime supplies these functions.
    struct image_t {
      unsigned width, height;
      int pitch, pixel_format, component_type;
      std::uint8_t pixel_bytes, component_bytes, num_components, planar;
      std::uint8_t gpu_mem, colorspace, reserved[2];
      void *pixels, *delete_ptr, *delete_proc;
      std::uint64_t buffer_bytes;
    };

    static_assert(sizeof(image_t) == 64);

    struct api_t {
      int (*create)(const char *, void **);
      int (*destroy)(void *);
      int (*set_image)(void *, const char *, image_t *);
      int (*set_u32)(void *, const char *, unsigned);
      int (*set_stream)(void *, const char *, void *);
      int (*create_stream)(void **);
      int (*destroy_stream)(void *);
      int (*load)(void *);
      int (*run)(void *, int);
      int (*alloc)(image_t *, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);
      void (*dealloc)(image_t *);
      const char *(*error)(int);
      int (*synchronize)(void *);
      std::vector<void *> libraries;

      void *open(const std::filesystem::path &path) {
        void *library = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!library) {
          throw std::runtime_error("RTX HDR: cannot load " + path.string() + ": " + dlerror());
        }
        libraries.push_back(library);
        return library;
      }

      template<class T>
      void bind(T &function, void *library, const char *name) {
        function = reinterpret_cast<T>(dlsym(library, name));
        if (!function) {
          throw std::runtime_error(std::string("RTX HDR: missing runtime symbol ") + name);
        }
      }

      explicit api_t(const std::filesystem::path &root) {
        const auto libs = root / "nvvfx/libs";
        // Preserve the wheel layout and its hashed CUDA-runtime SONAME. Load
        // dependencies before effect adapters; never alter LD_LIBRARY_PATH.
        const auto cudart = root / "nvidia_vfx.libs/libcudart-256e6409.so.12.9.79";
        open(cudart);
        for (const char *name : {"libnppc.so.12", "libnppial.so.12", "libnppicc.so.12", "libnppidei.so.12", "libnppif.so.12", "libnppig.so.12", "libnppim.so.12", "libnppist.so.12", "libnppitc.so.12", "libcudnn.so.9"}) {
          open(libs / name);
        }
        void *cv = open(libs / "libNVCVImage.so");
        open(libs / "libnvngxruntime.so");
        open(libs / "libVideoFXLocal.so");
        void *fx = open(libs / "libVideoFX.so");
        open(libs / "libnvVFXTrueHDR.so");
        open(libs / "libnvidia-ngx-truehdr.so.1.2.3");
        bind(create, fx, "NvVFX_CreateEffect");
        bind(destroy, fx, "NvVFX_DestroyEffect");
        bind(set_image, fx, "NvVFX_SetImage");
        bind(set_u32, fx, "NvVFX_SetU32");
        bind(set_stream, fx, "NvVFX_SetCudaStream");
        bind(create_stream, fx, "NvVFX_CudaStreamCreate");
        bind(destroy_stream, fx, "NvVFX_CudaStreamDestroy");
        bind(load, fx, "NvVFX_Load");
        bind(run, fx, "NvVFX_Run");
        bind(alloc, cv, "NvCVImage_Alloc");
        bind(dealloc, cv, "NvCVImage_Dealloc");
        bind(error, cv, "NvCV_GetErrorStringFromCode");
        bind(synchronize, open("libcuda.so.1"), "cuStreamSynchronize");
      }

      void check(const char *operation, int status) {
        if (status) {
          const char *message = error(status);
          throw std::runtime_error(std::string("RTX HDR: ") + operation + ": " + (message ? message : "unknown NVIDIA status"));
        }
      }
    };

    api_t &api(const std::filesystem::path &root) {
      // The SDK registers process-wide CUDA callbacks. Keep libraries loaded
      // until process exit; effects/images are still destroyed per session.
      static std::mutex mutex;
      static api_t *loaded = nullptr;
      static std::filesystem::path loaded_root;
      std::lock_guard lock(mutex);
      if (loaded && root != loaded_root) {
        throw std::runtime_error("RTX HDR: another runtime is already loaded");
      }
      if (!loaded) {
        loaded = new api_t(root);
        loaded_root = root;
      }
      return *loaded;
    }
  }  // namespace

  void *create_stream(const std::filesystem::path &runtime) {
    auto &f = api(runtime);
    void *stream = nullptr;
    f.check("create CUDA stream", f.create_stream(&stream));
    return stream;
  }

  void destroy_stream(const std::filesystem::path &runtime, void *stream) {
    api(runtime).destroy_stream(stream);
  }

  struct effect_t::impl_t {
    api_t &functions;
    void *stream, *effect = nullptr;
    image_t src {}, dst {};
    void *peak_table = nullptr;
    std::optional<settings_t> applied;

    impl_t(const std::filesystem::path &root, void *stream):
        functions(api(root)),
        stream(stream) {}

    ~impl_t() {
      functions.synchronize(stream);
      if (peak_table) {
        destroy_peak_lut(peak_table);
      }
      if (effect) {
        functions.destroy(effect);
      }
      if (src.pixels) {
        functions.dealloc(&src);
      }
      if (dst.pixels) {
        functions.dealloc(&dst);
      }
    }

    void settings(const settings_t &value) {
      if (value.contrast < 0 || value.contrast > 200 || value.saturation < 0 || value.saturation > 200 || value.middle_gray < 10 || value.middle_gray > 100 || value.peak < 400 || value.peak > 2000) {
        throw std::runtime_error("RTX HDR: tuning value outside SDK limits");
      }
      if (applied == value) {
        return;
      }
      const auto native = native_settings(value);
      if (value.peak > 1000) {
        if (!peak_table && create_peak_lut(&peak_table)) {
          throw std::runtime_error("RTX HDR: cannot allocate peak correction table");
        }
        const auto table = peak_lut(value);
        if (upload_peak_lut(peak_table, table.data())) {
          throw std::runtime_error("RTX HDR: cannot upload peak correction table");
        }
      }
      functions.check("Contrast", functions.set_u32(effect, "Contrast", value.contrast));
      functions.check("Saturation", functions.set_u32(effect, "Saturation", value.saturation));
      functions.check("MiddleGray", functions.set_u32(effect, "MiddleGray", native.middle_gray));
      functions.check("Luminance", functions.set_u32(effect, "Luminance", native.peak));
      applied = value;
    }
  };

  effect_t::effect_t(const std::filesystem::path &root, unsigned width, unsigned height, void *stream) {
    if (width < 16 || height < 16 || width > 8192 || height > 8192) {
      throw std::runtime_error("RTX HDR: frame dimensions must be between 16 and 8192");
    }
    impl = std::make_unique<impl_t>(root, stream);
    auto &f = impl->functions;
    f.check("create TrueHDR", f.create("TrueHDR", &impl->effect));
    f.check("set stream", f.set_stream(impl->effect, "CudaStream", stream));
    f.check("allocate RGBA8 input", f.alloc(&impl->src, width, height, 6, 1, 0, 1, 1));
    f.check("allocate RGB10A2 output", f.alloc(&impl->dst, width, height, 13, 11, 0, 1, 1));
    f.check("set input", f.set_image(impl->effect, "SrcImage0", &impl->src));
    f.check("set output", f.set_image(impl->effect, "DstImage0", &impl->dst));
    impl->settings({});
    f.check("enable debanding", f.set_u32(impl->effect, "DebandingOff", 0));
    f.check("load TrueHDR", f.load(impl->effect));
  }

  effect_t::~effect_t() = default;

  void effect_t::run(const settings_t &settings) {
    if (impl->applied != settings && impl->functions.synchronize(impl->stream)) {
      throw std::runtime_error("RTX HDR: cannot drain the CUDA stream before updating tuning");
    }
    impl->settings(settings);
    impl->functions.check("run TrueHDR", impl->functions.run(impl->effect, 1));
    if (settings.peak > 1000 && apply_peak_lut(impl->dst.pixels, impl->dst.pitch, impl->dst.width, impl->dst.height, impl->peak_table, impl->stream)) {
      throw std::runtime_error("RTX HDR: peak correction failed");
    }
  }

  std::uint64_t effect_t::input() const {
    return reinterpret_cast<std::uintptr_t>(impl->src.pixels);
  }

  std::uint64_t effect_t::output() const {
    return reinterpret_cast<std::uintptr_t>(impl->dst.pixels);
  }

  unsigned effect_t::input_pitch() const {
    return impl->src.pitch;
  }

  unsigned effect_t::output_pitch() const {
    return impl->dst.pitch;
  }
}  // namespace platf::linux_truehdr
