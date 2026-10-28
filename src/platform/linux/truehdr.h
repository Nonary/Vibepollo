/** Native NVIDIA VFX TrueHDR. All buffers and the CUDA stream remain GPU resident. */
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace platf::linux_truehdr {
  struct settings_t {
    int contrast = 100;
    int saturation = 100;
    int middle_gray = 50;
    int peak = 1000;
    bool operator==(const settings_t &) const = default;
  };

  // SDK initialization is required for CUDA-only consumers without GL interop.
  void *create_stream(const std::filesystem::path &runtime);
  void destroy_stream(const std::filesystem::path &runtime, void *stream);

  class effect_t {
  public:
    // Caller owns the current CUDA context and stream, which must outlive this
    // object. Destruction drains the stream before releasing SDK resources.
    effect_t(const std::filesystem::path &runtime, unsigned width, unsigned height, void *stream);
    ~effect_t();
    effect_t(const effect_t &) = delete;
    effect_t &operator=(const effect_t &) = delete;
    void run(const settings_t &settings);
    std::uint64_t input() const;
    std::uint64_t output() const;
    unsigned input_pitch() const;
    unsigned output_pitch() const;

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl;
  };
}  // namespace platf::linux_truehdr
