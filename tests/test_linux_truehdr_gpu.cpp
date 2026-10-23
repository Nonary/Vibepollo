/** Real CUDA/GL/SDK pixels, live tuning and orderly context teardown. */
#include "src/platform/linux/truehdr_gl.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <glad/egl.h>
#include <iostream>
#include <stdexcept>
#include <vector>

static void require(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}

static double nits(unsigned code) {
  const double p = std::pow(code / 1023.0, 32.0 / 2523.0);
  return 10000 * std::pow(std::max(p - 3424.0 / 4096, 0.0) / (2413.0 / 128 - 2392.0 / 128 * p), 16384.0 / 2610);
}

static unsigned corrected_code(unsigned code, int peak, int gray) {
  double value = nits(code);
  const double gain = peak / 1000.0;
  if (gray < 10 * gain) {
    value = value <= 10 ? value * gray / 10 : gray + (value - 10) * (peak - gray) / 990;
  } else {
    value *= gain;
  }
  const double x = std::pow(std::clamp(value / 10000, 0.0, 1.0), 2610.0 / 16384);
  return std::lround(1023 * std::pow((3424.0 / 4096 + 2413.0 / 128 * x) / (1 + 2392.0 / 128 * x), 2523.0 / 32));
}

static void cu(CUresult status) {
  require(status == CUDA_SUCCESS, "CUDA operation failed");
}

int main(int argc, char **argv) {
  CudaFunctions *cuda = nullptr;
  CUcontext context = nullptr;
  CUstream stream = nullptr;
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext glcontext = EGL_NO_CONTEXT;
  int result = 1;
  try {
    require(argc == 2 || argc == 3, "runtime directory is required");
    if (cuda_load_functions(&cuda, nullptr) || cuda->cuInit(0) != CUDA_SUCCESS) {
      return 77;
    }
    CUdevice device = 0;
    if (cuda->cuDeviceGet(&device, 0) != CUDA_SUCCESS) {
      cuda_free_functions(&cuda);
      return 77;
    }
    require(gladLoaderLoadEGL(EGL_NO_DISPLAY), "EGL loader unavailable");
    using query_t = EGLBoolean (*)(EGLint, void **, EGLint *);
    const auto query = reinterpret_cast<query_t>(eglGetProcAddress("eglQueryDevicesEXT"));
    require(query, "EGL device enumeration unavailable");
    std::array<void *, 16> devices {};
    EGLint count = 0;
    require(query(devices.size(), devices.data(), &count), "EGL device query failed");
    GladGLContext gl {};
    for (int i = 0; i < count; ++i) {
      display = eglGetPlatformDisplayEXT(0x313F, devices[i], nullptr);  // EGL_PLATFORM_DEVICE_EXT
      if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
        continue;
      }
      require(gladLoaderLoadEGL(display), "Initialized EGL dispatch unavailable");
      const EGLint attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
      EGLConfig config = nullptr;
      EGLint configs = 0;
      if (!eglChooseConfig(display, attributes, &config, 1, &configs) || !configs || !eglBindAPI(EGL_OPENGL_API)) {
        eglTerminate(display);
        continue;
      }
      glcontext = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
      if (glcontext != EGL_NO_CONTEXT && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, glcontext) && gladLoaderLoadGLContext(&gl) && std::string(reinterpret_cast<const char *>(gl.GetString(GL_VENDOR))).find("NVIDIA") != std::string::npos) {
        break;
      }
      if (glcontext != EGL_NO_CONTEXT) {
        eglDestroyContext(display, glcontext);
      }
      glcontext = EGL_NO_CONTEXT;
      eglTerminate(display);
      display = EGL_NO_DISPLAY;
    }
    require(glcontext != EGL_NO_CONTEXT, "No NVIDIA EGL context");
    if (argc == 3) {
      cu(cuda->cuCtxCreate(&context, 0, device));
    } else {
      cu(cuda->cuDevicePrimaryCtxRetain(&context, device));
      cu(cuda->cuCtxPushCurrent(context));
    }
    cu(cuda->cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING));
    constexpr unsigned width = 128, height = 64;
    std::vector<unsigned char> rgba(width * height * 4);
    for (unsigned y = 0; y < height; ++y) {
      for (unsigned x = 0; x < width; ++x) {
        auto *p = &rgba[(y * width + x) * 4];
        p[0] = x * 255 / (width - 1);
        p[1] = y * 255 / (height - 1);
        p[2] = 64;
        p[3] = 255;
        if (y >= height - 16) {
          const unsigned char value = x < 16 ? 128 : x < 32 ? 0 : 255;
          p[0] = p[1] = p[2] = value;
        }
      }
    }
    GLuint source = 0, lut = 0, framebuffer = 0;
    gl.GenTextures(1, &source);
    gl.BindTexture(GL_TEXTURE_2D, source);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    gl.GenTextures(1, &lut);
    gl.BindTexture(GL_TEXTURE_2D, lut);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    const std::array<unsigned short, 6> identity {0, 0, 0, 65535, 65535, 65535};
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGB16, 2, 1, 0, GL_RGB, GL_UNSIGNED_SHORT, identity.data());
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.GenFramebuffers(1, &framebuffer);
    {
      using namespace platf::linux_truehdr;
      gl_converter_t converter(gl, *cuda, stream, argv[1], width, height);
      auto read = [&](bool enabled, int brightness, settings_t settings) {
        const GLuint texture = converter.convert(source, lut, enabled, brightness, settings);
        gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        std::vector<std::uint32_t> pixels(width * height);
        gl.ReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, pixels.data());
        require(gl.GetError() == GL_NO_ERROR, "HDR readback failed");
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        return pixels;
      };
      const auto original = read(true, 0, {});
      // Compare every GPU-interoperated pixel to the native SDK output for
      // the identical input. This catches ordering, pitch and packing errors.
      effect_t reference(argv[1], width, height, stream);
      CUDA_MEMCPY2D copy {};
      copy.srcMemoryType = CU_MEMORYTYPE_HOST;
      copy.srcHost = rgba.data();
      copy.srcPitch = width * 4;
      copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
      copy.dstDevice = reference.input();
      copy.dstPitch = reference.input_pitch();
      copy.WidthInBytes = width * 4;
      copy.Height = height;
      cu(cuda->cuMemcpy2DAsync(&copy, stream));
      std::vector<std::uint32_t> expected(width * height);
      copy = {};
      copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
      copy.srcDevice = reference.output();
      copy.srcPitch = reference.output_pitch();
      copy.dstMemoryType = CU_MEMORYTYPE_HOST;
      copy.dstHost = expected.data();
      copy.dstPitch = width * 4;
      copy.WidthInBytes = width * 4;
      copy.Height = height;
      auto compare_native = [&](const std::vector<std::uint32_t> &actual, settings_t settings) {
        gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, converter.prepared_sdr_texture(), 0);
        std::vector<unsigned char> prepared(rgba.size());
        gl.ReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, prepared.data());
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        for (std::size_t i = 0; i < rgba.size(); ++i) {
          if (prepared[i] != rgba[i]) {
            std::cerr << "SDR byte " << i << " actual " << unsigned(prepared[i]) << " expected " << unsigned(rgba[i]) << '\n';
            throw std::runtime_error("SDR preparation changed identity-LUT source bytes");
          }
        }
        CUDA_MEMCPY2D input {};
        input.srcMemoryType = CU_MEMORYTYPE_HOST;
        input.srcHost = rgba.data();
        input.srcPitch = width * 4;
        input.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        input.dstDevice = reference.input();
        input.dstPitch = reference.input_pitch();
        input.WidthInBytes = width * 4;
        input.Height = height;
        cu(cuda->cuMemcpy2DAsync(&input, stream));
        auto native = settings;
        if (settings.peak > 1000) {
          native.peak = 1000;
          native.middle_gray = std::clamp(int(std::lround(settings.middle_gray * 1000.0 / settings.peak)), 10, 100);
        }
        reference.run(native);
        cu(cuda->cuMemcpy2DAsync(&copy, stream));
        cu(cuda->cuStreamSynchronize(stream));
        for (std::size_t i = 0; i < expected.size(); ++i) {
          if (settings.peak > 1000) {
            unsigned mapped = expected[i] & 0xc0000000u;
            for (unsigned shift : {0u, 10u, 20u}) {
              mapped |= corrected_code((expected[i] >> shift) & 1023u, settings.peak, settings.middle_gray) << shift;
            }
            expected[i] = mapped;
          }
          if ((actual[i] & 0x3fffffff) != (expected[i] & 0x3fffffff)) {
            std::cerr << "settings " << settings.contrast << ',' << settings.saturation << ',' << settings.middle_gray << ',' << settings.peak
                      << " pixel " << i << " actual " << actual[i] << " expected " << expected[i] << '\n';
            throw std::runtime_error("Interop pixels differ from native TrueHDR");
          }
        }
      };
      compare_native(original, {});
      for (auto settings : {settings_t {150, 100, 50, 1000}, settings_t {100, 150, 50, 1000}, settings_t {100, 100, 80, 1000}, settings_t {100, 100, 50, 1200}, settings_t {100, 100, 50, 1600}, settings_t {100, 100, 50, 2000}, settings_t {100, 100, 10, 2000}}) {
        auto baseline_settings = settings;
        baseline_settings.peak = 1000;
        // Let native exposure settle while keeping identical reference history.
        std::vector<std::uint32_t> baseline, changed;
        for (int frame = 0; frame < 12; ++frame) {
          baseline = read(true, 0, baseline_settings);
          compare_native(baseline, baseline_settings);
        }
        for (int frame = 0; frame < 12; ++frame) {
          changed = read(true, 0, settings);
          compare_native(changed, settings);
        }
        require(changed != original, "Live tuning did not change HDR pixels");
        if (settings.peak > 1000) {
          const auto luminance = [&](unsigned x) { return nits(changed[(height - 8) * width + x] & 1023u); };
          std::cout << "peak " << settings.peak << " gray " << settings.middle_gray << ": highlight " << luminance(width - 8) << " gray " << luminance(8) << " nits\n";
          require(std::abs(luminance(width - 8) / settings.peak - 1) < 0.04, "Peak failed to reach requested luminance");
          const double baseline_gray = nits(baseline[(height - 8) * width + 8] & 1023u);
          std::cout << "baseline gray " << baseline_gray << " nits\n";
          if (settings.middle_gray >= settings.peak / 100.0) {
            require(std::abs(luminance(8) / baseline_gray - 1) < 0.12, "Peak correction brightened middle gray");
          } else {
            // Native scene-dependent gray need not equal the slider's anchor.
            // The floor correction must avoid the uniform-gain doubling.
            require(luminance(8) < baseline_gray * settings.peak / 1000.0 * 0.9, "Gray floor used uniform peak gain");
          }
          require(luminance(24) < 0.01, "Peak correction raised black");
        }
      }
      const settings_t high {100, 100, 50, 2000};
      compare_native(read(true, 0, high), high);
      compare_native(read(true, 0, high), high);
      // Compare the same native frame history when restoring live values.
      compare_native(read(true, 0, {}), {});
      require(read(false, 0, {}) != original, "Disabling RTX HDR did not select the SDR fallback");
      require(read(false, 0, {}) != read(false, 100, {}), "SDR brightness did not change fallback pixels");
      bool rejected = false;
      try {
        converter.convert(source, lut, true, 0, {201, 100, 50, 1000});
      } catch (const std::exception &) {
        rejected = true;
      }
      require(rejected, "Invalid tuning was accepted");
      compare_native(read(true, 0, {}), {});
    }
    gl.DeleteTextures(1, &source);
    gl.DeleteTextures(1, &lut);
    gl.DeleteFramebuffers(1, &framebuffer);
    std::cout << "Native TrueHDR pixels, GL/CUDA interop, live tuning, fallback and cleanup passed\n";
    result = 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  if (stream) {
    cuda->cuStreamDestroy(stream);
  }
  if (context) {
    CUcontext popped = nullptr;
    cuda->cuCtxPopCurrent(&popped);
    if (argc == 3) {
      cuda->cuCtxDestroy(context);
    } else {
      cuda->cuDevicePrimaryCtxRelease(0);
    }
  }
  if (display != EGL_NO_DISPLAY) {
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (glcontext != EGL_NO_CONTEXT) {
      eglDestroyContext(display, glcontext);
    }
    eglTerminate(display);
  }
  if (cuda) {
    cuda_free_functions(&cuda);
  }
  return result;
}
