#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace video::policy {
  struct rational_t {
    int numerator;
    int denominator;
    friend bool operator==(const rational_t &, const rational_t &) = default;
  };

  rational_t framerate_x100_to_rational(std::int32_t value);

  struct encoder_requirements_t {
    bool hdr = false;
    bool yuv444 = false;
  };
  struct encoder_capabilities_t {
    bool available = false;
    bool hdr = false;
    bool yuv444 = false;
  };
  class encoder_capability_provider_t {
  public:
    virtual ~encoder_capability_provider_t() = default;
    virtual encoder_capabilities_t capabilities(std::string_view encoder) const = 0;
  };

  std::optional<std::string> select_encoder(
    std::span<const std::string_view> preference,
    encoder_requirements_t requirements,
    const encoder_capability_provider_t &provider
  );
}  // namespace video::policy
