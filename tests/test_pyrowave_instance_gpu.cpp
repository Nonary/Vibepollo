/** Reconnect/reinit lifetime coverage using real Linux PyroWave Vulkan devices. */
#include "src/platform/linux/pyrowave_core.h"

#include <iostream>
#include <stdexcept>

namespace {
  void encode(pyrowave::linux_gpu::core_t &core) {
    std::vector<std::uint8_t> pixels(64 * 64 * 4, 128), bits;
    std::vector<pyrowave::linux_gpu::packet_t> packets;
    pyrowave::linux_gpu::source_t source;
    source.pixels = pixels.data();
    source.width = source.height = 64;
    source.stride = 64 * 4;
    std::string error;
    if (!core.encode(source, 65536, UINT32_MAX, bits, packets, error)) {
      throw std::runtime_error(error);
    }
    if (bits.empty() || packets.empty()) {
      throw std::runtime_error("recreated encoder produced no frame");
    }
  }
}  // namespace

int main(int argc, char **argv) {
  try {
    pyrowave::linux_gpu::config_t config;
    config.width = config.height = 64;
    config.matrix = {0.2126f, 0.7152f, 0.0722f, 0.0f, -0.1146f, -0.3854f, 0.5f, 0.5f, 0.5f, -0.4542f, -0.0458f, 0.5f};
    if (argc > 1) {
      config.render_device = argv[1];
    }
    std::string error;
    auto first = pyrowave::linux_gpu::core_t::create(config, error);
    if (!first) {
      if (argc == 1 && (error == "no compatible Vulkan GPU for capture device" || error == "no Vulkan devices" || error == "Vulkan loader unavailable" || error == "Vulkan instance creation failed")) {
        return 77;
      }
      throw std::runtime_error(error);
    }
    encode(*first);
    // Keep another live device while releasing/recreating its sibling. This
    // exercises the shared instance across probes, disconnects and reinit.
    for (int reconnect = 0; reconnect < 4; ++reconnect) {
      auto second = pyrowave::linux_gpu::core_t::create(config, error);
      if (!second) {
        throw std::runtime_error(error);
      }
      encode(*second);
      first.reset();
      encode(*second);
      first = pyrowave::linux_gpu::core_t::create(config, error);
      if (!first) {
        throw std::runtime_error(error);
      }
      encode(*first);
      second.reset();
      encode(*first);
    }
    first.reset();
    pyrowave::linux_gpu::core_t::shutdown();
    pyrowave::linux_gpu::core_t::shutdown();
    if (pyrowave::linux_gpu::core_t::create(config, error) || error != "PyroWave is shutting down") {
      throw std::runtime_error("process shutdown allowed a new encoder device");
    }
    std::cout << "PyroWave device reconnects and shared instance shutdown passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
