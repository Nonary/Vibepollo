#pragma once

#include <optional>
#include <string_view>
#include <utility>

namespace amf::lifecycle {
  enum class initialization_failure_e {
    none,
    encoder_create,
    encoder_init,
    unsupported_format,
    device_or_capture,
    conversion,
    network,
    cancelled,
    busy,
    unsafe_runtime,
  };

  template<typename T>
  struct initialization_result_t {
    std::optional<T> value;
    initialization_failure_e failure = initialization_failure_e::none;
  };

  // Owned by one stream. A failed rollback ends that stream; the next stream
  // starts with native AMF again. Explicit implementations never switch.
  struct initialization_fallback_t {
    bool legacy = false;

    template<typename Native, typename Legacy, typename Log>
    auto initialize(bool automatic, bool compatible, Native &&native, Legacy &&rollback, Log &&log) {
      if (legacy) return rollback();
      auto result = native();
      if (result.value || !automatic || !compatible ||
          (result.failure != initialization_failure_e::encoder_create &&
           result.failure != initialization_failure_e::encoder_init)) return result;
      // The native attempt publishes failure only after destroying its resources.
      // Set this before rollback so a failed rollback cannot loop back to native.
      legacy = true;
      log(result.failure);
      return rollback();
    }
  };

  inline constexpr bool legacy_stream_format_compatible(
    int codec, int chroma, bool ten_bit, bool intra_refresh, bool native_options) {
    return codec >= 0 && codec <= 2 && chroma == 0 &&
           !(codec == 0 && ten_bit) && !intra_refresh && !native_options;
  }
}
