/** Linux GPU integration test: real conversion, encoding, packetization and decoding. */
#include "src/platform/linux/pyrowave_core.h"

#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <gbm.h>
#include <iostream>
#include <linux/dma-buf.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

// PyroWave requires Vulkan types before its C API.
#include <pyrowave.h>

static void require(bool ok, const char *message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

static std::array<float, 3> sdr_to_pq(std::array<float, 3> rgb) {
  for (auto &c : rgb) {
    c /= 255;
    c = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
  }
  std::array<float, 3> result = {
    0.627404f * rgb[0] + 0.329283f * rgb[1] + 0.043313f * rgb[2],
    0.069097f * rgb[0] + 0.919540f * rgb[1] + 0.011362f * rgb[2],
    0.016391f * rgb[0] + 0.088013f * rgb[1] + 0.895595f * rgb[2]
  };
  for (auto &c : result) {
    float x = std::pow(c / 100.0f, 2610.0f / 16384.0f);
    c = 255 * std::pow((3424.0f / 4096.0f + 2413.0f / 128.0f * x) / (1 + 2392.0f / 128.0f * x), 2523.0f / 32.0f);
  }
  return result;
}

struct dma_source_t {
  int fd = -1;
  gbm_device *device = nullptr;
  gbm_bo *bo = nullptr;
  egl::surface_descriptor_t surface;

  ~dma_source_t() {
    egl::reset_surface(surface);
    if (bo) {
      gbm_bo_destroy(bo);
    }
    if (device) {
      gbm_device_destroy(device);
    }
    if (fd >= 0) {
      close(fd);
    }
  }

  void init(const char *path, bool ten_bit) {
    fd = open(path, O_RDWR | O_CLOEXEC);
    require(fd >= 0, "opening test DRM device failed");
    device = gbm_create_device(fd);
    require(device, "creating GBM device failed");
    bo = gbm_bo_create(device, 64, 128, ten_bit ? GBM_FORMAT_XRGB2101010 : GBM_FORMAT_XRGB8888, GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE);
    if (!bo) {
      bo = gbm_bo_create(device, 64, 128, ten_bit ? GBM_FORMAT_XRGB2101010 : GBM_FORMAT_XRGB8888, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
    }
    require(bo, "allocating test DMA-BUF failed");
    std::uint32_t stride = 0;
    void *map_data = nullptr;
    auto *pixels = static_cast<std::uint8_t *>(gbm_bo_map(bo, 0, 0, 64, 128, GBM_BO_TRANSFER_WRITE, &stride, &map_data));
    require(pixels, "mapping test DMA-BUF failed");
    for (unsigned y = 0; y < 128; ++y) {
      for (unsigned x = 0; x < 64; ++x) {
        auto *p = pixels + y * stride + x * 4;
        p[0] = y < 64 ? 64 : 32;
        p[1] = y < 64 ? 160 : 96;
        p[2] = y < 64 ? 16 : 192;
        p[3] = 255;
        if (ten_bit) {
          std::uint32_t pixel = (std::uint32_t(p[2]) * 4 << 20) | (std::uint32_t(p[1]) * 4 << 10) | (std::uint32_t(p[0]) * 4);
          std::memcpy(p, &pixel, 4);
        }
      }
    }
    gbm_bo_unmap(bo, map_data);
    surface.width = 64;
    surface.height = 128;
    surface.fourcc = gbm_bo_get_format(bo);
    surface.modifier = gbm_bo_get_modifier(bo);
    int planes = gbm_bo_get_plane_count(bo);
    require(planes > 0 && planes <= 4, "invalid GBM plane count");
    for (int i = 0; i < planes; ++i) {
      surface.fds[i] = gbm_bo_get_fd_for_plane(bo, i);
      surface.pitches[i] = gbm_bo_get_stride_for_plane(bo, i);
      surface.offsets[i] = gbm_bo_get_offset(bo, i);
      require(surface.fds[i] >= 0, "exporting test DMA-BUF failed");
    }
  }
};

int main(int argc, char **argv) {
  try {
    // Direct libvulkan entry points must remain functions, not resolve to
    // the statically linked codec's Volk function-pointer variables.
    VkInstanceCreateInfo native_info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance native_instance = VK_NULL_HANDLE;
    auto native_result = vkCreateInstance(&native_info, nullptr, &native_instance);
    if (native_result != VK_SUCCESS) {
      return 77;
    }
    vkDestroyInstance(native_instance, nullptr);
    pyrowave_device device = nullptr;
    if (pyrowave_create_default_device(&device) != PYROWAVE_SUCCESS) {
      std::cout << "SKIP: no Vulkan device\n";
      return 77;
    }

    struct device_guard {
      pyrowave_device d;

      ~device_guard() {
        pyrowave_device_destroy(d);
      }
    } guard {device};

    dma_source_t dma;
    if (argc > 1) {
      dma.init(argv[1], argc > 2);
    }
    unsigned cases = 0;
    for (bool chroma444 : {false, true}) {
      for (unsigned depth : {8u, 10u}) {
        for (bool full : {false, true}) {
          for (bool hdr : {false, true}) {
            if (hdr && depth == 8) {
              continue;
            }
            pyrowave::linux_gpu::config_t config;
            if (argc > 1) {
              config.render_device = argv[1];
            }
            config.width = 128;
            config.height = 128;
            config.yuv444 = chroma444;
            config.ten_bit = depth > 8;
            config.hdr = hdr;
            float max = float((1u << depth) - 1), scale = float(1u << (depth - 8));
            float ym = full ? 1.0f : 219 * scale / max, ya = full ? 0.0f : 16 * scale / max;
            float cm = full ? 1.0f : 224 * scale / max, ca = float(1u << (depth - 1)) / max;
            float kr = hdr ? 0.2627f : 0.2126f, kb = hdr ? 0.0593f : 0.0722f, kg = 1 - kr - kb;
            config.matrix = {kr * ym, kg * ym, kb * ym, ya, -0.5f * kr / (1 - kb) * cm, -0.5f * kg / (1 - kb) * cm, 0.5f * cm, ca, 0.5f * cm, -0.5f * kg / (1 - kr) * cm, -0.5f * kb / (1 - kr) * cm, ca};
            std::string error;
            auto core = pyrowave::linux_gpu::core_t::create(config, error);
            if (!core) {
              if (argc == 1 && error == "no compatible Vulkan GPU for capture device") return 77;
              throw std::runtime_error(error);
            }
            pyrowave_decoder decoder = nullptr;
            pyrowave_decoder_create_info info {};
            info.device = device;
            info.width = info.height = 128;
            info.chroma = chroma444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
            require(pyrowave_decoder_create(&info, &decoder) == PYROWAVE_SUCCESS, "decoder creation failed");

            struct decoder_guard {
              pyrowave_decoder d;

              ~decoder_guard() {
                pyrowave_decoder_destroy(d);
              }
            } dg {decoder};

            // Padded rows and scaling exercise upload pitch and the conversion geometry.
            std::vector<std::uint8_t> pixels(272 * 128);
            for (int y = 0; y < 128; ++y) {
              for (int x = 0; x < 64; ++x) {
                auto i = y * 272 + x * 4;
                pixels[i] = y < 64 ? 64 : 32;
                pixels[i + 1] = y < 64 ? 160 : 96;
                pixels[i + 2] = y < 64 ? 16 : 192;
                pixels[i + 3] = 255;
              }
            }
            pyrowave::linux_gpu::source_t source;
            if (dma.bo) {
              source.surface = &dma.surface;
            }
            source.pixels = pixels.data();
            source.width = 64;
            source.height = 128;
            source.stride = 272;
            const std::vector<std::array<std::uint16_t, 3>> inverted_lut {{{65535, 65535, 65535}}, {{0, 0, 0}}};
            const std::uint8_t cursor[] = {32, 0, 64, 128};
            for (int variant = 0; variant < 4; ++variant) {
              for (auto boundary : {std::size_t(UINT32_MAX), std::size_t(1024)}) {
                source.y_invert = variant == 1;
                source.lut = variant == 2 ? &inverted_lut : nullptr;
                source.cursor = variant == 3 ? cursor : nullptr;
                source.cursor_width = source.cursor_height = 1;
                source.cursor_dst_width = source.width;
                source.cursor_dst_height = source.height;
                std::vector<std::uint8_t> bits;
                std::vector<pyrowave::linux_gpu::packet_t> packets;
                require(core->encode(source, 65536, boundary, bits, packets, error), error.c_str());
                pyrowave_decoder_clear(decoder);
                for (auto &packet : packets) {
                  require(packet.offset + packet.size <= bits.size(), "packet outside bitstream");
                  require(pyrowave_decoder_push_packet(decoder, bits.data() + packet.offset, packet.size) == PYROWAVE_SUCCESS, "packet rejected");
                }
                require(pyrowave_decoder_decode_is_ready(decoder, false), "encoded frame is incomplete");
                pyrowave_cpu_buffer out {};
                out.width = out.height = 128;
                out.format = chroma444 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
                std::vector<std::uint8_t> planes[3];
                for (int i = 0; i < 3; ++i) {
                  unsigned dim = (i && !chroma444) ? 64 : 128;
                  planes[i].resize(dim * dim);
                  out.data[i] = planes[i].data();
                  out.row_stride_in_bytes[i] = dim;
                  out.plane_size_in_bytes[i] = planes[i].size();
                }
                require(pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &out) == PYROWAVE_SUCCESS, "decode failed");
                for (int i = 0; i < 3; ++i) {
                  auto *m = config.matrix.data() + i * 4;
                  std::array<float, 3> rgb = {192.0f, 96.0f, 32.0f};
                  if (variant == 1) {
                    rgb[0] = 16;
                    rgb[1] = 160;
                    rgb[2] = 64;
                  }
                  if (variant == 2) {
                    for (auto &v : rgb) {
                      v = 255 - v;
                    }
                  }
                  if (hdr && argc <= 2) {
                    rgb = sdr_to_pq(rgb);
                  }
                  if (variant == 3) {
                    auto c = hdr ? sdr_to_pq({64 * 255.0f / 128, 0, 32 * 255.0f / 128}) : std::array<float, 3> {64 * 255.0f / 128, 0, 32 * 255.0f / 128};
                    for (int j = 0; j < 3; ++j) {
                      rgb[j] = c[j] * (128.0f / 255) + rgb[j] * (127.0f / 255);
                    }
                  }
                  float expected = m[0] * rgb[0] + m[1] * rgb[1] + m[2] * rgb[2] + 255 * m[3];
                  auto dim = out.row_stride_in_bytes[i];
                  auto actual = planes[i][dim * (dim * 3 / 4) + dim / 2];
                  if (std::abs(actual - expected) > 3) {
                    std::cerr << "plane " << i << " expected " << expected << " got " << unsigned(actual) << '\n';
                    throw std::runtime_error("decoded color mismatch");
                  }
                }
                ++cases;
              }
            }
          }
        }
      }
    }
    std::cout << cases << " Linux GPU encode/decode cases passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
