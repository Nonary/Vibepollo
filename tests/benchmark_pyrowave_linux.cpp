// Manual end-to-end benchmark; timings include DMA-BUF import/fallback,
// Vulkan preprocessing, native TrueHDR, encode, GPU readback and packetization.
// Run without other GPU benchmarks; record game/GPU load alongside the result.
#include "src/platform/linux/pyrowave_core.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <gbm.h>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <unistd.h>

int main(int argc, char **argv) {
  try {
    if (argc < 3) {
      std::cerr << "Usage: benchmark_pyrowave_linux DRM_NODE TRUEHDR_RUNTIME_OR_DASH [FRAMES] [cpu-sync]\n";
      return 2;
    }
    const int width = 3840, height = 2160, frames = argc > 3 ? std::stoi(argv[3]) : 60;
    if (frames < 1 || frames > 100000) {
      throw std::runtime_error("invalid frame count");
    }
    int fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      throw std::runtime_error("DRM open failed");
    }
    auto *device = gbm_create_device(fd);
    auto *bo = gbm_bo_create(device, width, height, GBM_FORMAT_XRGB8888, GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE);
    if (!bo) {
      bo = gbm_bo_create(device, width, height, GBM_FORMAT_XRGB8888, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
    }
    if (!bo) {
      throw std::runtime_error("GBM allocation failed");
    }
    std::uint32_t stride = 0;
    void *mapped = nullptr;
    auto *pixels = static_cast<std::uint8_t *>(gbm_bo_map(bo, 0, 0, width, height, GBM_BO_TRANSFER_WRITE, &stride, &mapped));
    if (!pixels) {
      throw std::runtime_error("GBM map failed");
    }
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        auto *p = pixels + y * stride + x * 4;
        p[0] = std::uint8_t(x + y);
        p[1] = y * 255 / height;
        p[2] = (x * 255 / width) ^ (((x / 64 ^ y / 64) & 1) ? 31 : 0);
        p[3] = 255;
      }
    }
    gbm_bo_unmap(bo, mapped);
    egl::surface_descriptor_t surface;
    surface.width = width;
    surface.height = height;
    surface.fourcc = gbm_bo_get_format(bo);
    surface.modifier = gbm_bo_get_modifier(bo);
    int planes = gbm_bo_get_plane_count(bo);
    for (int i = 0; i < planes; ++i) {
      surface.fds[i] = gbm_bo_get_fd_for_plane(bo, i);
      surface.pitches[i] = gbm_bo_get_stride_for_plane(bo, i);
      surface.offsets[i] = gbm_bo_get_offset(bo, i);
    }
    pyrowave::linux_gpu::config_t config;
    config.width = width;
    config.height = height;
    config.yuv444 = config.ten_bit = config.hdr = true;
    config.render_device = argv[1];
    if (std::string(argv[2]) != "-") {
      config.truehdr_runtime = argv[2];
    }
    config.truehdr_external_sync = argc <= 4 || std::string(argv[4]) != "cpu-sync";
    float ym = 876.0f / 1023, ya = 64.0f / 1023, cm = 896.0f / 1023, ca = 512.0f / 1023;
    float kr = .2627f, kb = .0593f, kg = 1 - kr - kb;
    config.matrix = {kr * ym, kg * ym, kb * ym, ya, -.5f * kr / (1 - kb) * cm, -.5f * kg / (1 - kb) * cm, .5f * cm, ca, .5f * cm, -.5f * kg / (1 - kr) * cm, -.5f * kb / (1 - kr) * cm, ca};
    std::string error;
    auto core = pyrowave::linux_gpu::core_t::create(config, error);
    if (!core) {
      throw std::runtime_error(error);
    }
    pyrowave::linux_gpu::source_t source;
    source.surface = &surface;
    source.width = width;
    source.height = height;
    source.truehdr_enabled = !config.truehdr_runtime.empty();
    std::vector<double> times;
    std::vector<std::uint8_t> bits;
    std::vector<pyrowave::linux_gpu::packet_t> packets;
    std::size_t bytes = 0;
    int over = 0;
    for (int i = -10; i < frames; ++i) {
      auto begin = std::chrono::steady_clock::now();
      if (!core->encode(source, 650000, UINT32_MAX, bits, packets, error)) {
        throw std::runtime_error(error);
      }
      auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
      if (i >= 0) {
        times.push_back(ms);
        for (const auto &packet : packets) {
          bytes += packet.size;
        }
        if (ms > 1000.0 / 120) {
          ++over;
        }
      }
    }
    std::sort(times.begin(), times.end());
    auto percentile = [&](double p) {
      return times[std::min(times.size() - 1, std::size_t(p * times.size()))];
    };
    std::cout << "{\"width\":3840,\"height\":2160,\"chroma\":\"444\",\"source\":\"linear-dmabuf\",\"truehdr\":" << (source.truehdr_enabled ? "true" : "false") << ",\"frames\":" << frames << ",\"wall_median_ms\":" << percentile(.5) << ",\"wall_p95_ms\":" << percentile(.95) << ",\"wall_p99_ms\":" << percentile(.99) << ",\"wall_max_ms\":" << times.back() << ",\"wall_mean_ms\":" << std::accumulate(times.begin(), times.end(), 0.0) / times.size() << ",\"over_120hz_budget\":" << over << ",\"average_frame_bytes\":" << bytes / frames << "}\n";
    std::cout << "external_timeline=" << core->using_truehdr_external_sync() << "\n";
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto &packet : packets) {
      for (std::size_t i = packet.offset; i < packet.offset + packet.size; ++i) {
        hash ^= bits.at(i);
        hash *= 1099511628211ull;
      }
    }
    std::cout << "bitstream_fnv1a64=" << hash << "\n";
    core.reset();
    pyrowave::linux_gpu::core_t::shutdown();
    egl::reset_surface(surface);
    gbm_bo_destroy(bo);
    gbm_device_destroy(device);
    close(fd);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
