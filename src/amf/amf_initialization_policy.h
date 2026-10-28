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

  inline constexpr std::string_view initialization_failure_name(initialization_failure_e failure) {
    switch (failure) {
      case initialization_failure_e::none: return "none";
      case initialization_failure_e::encoder_create: return "encoder creation";
      case initialization_failure_e::encoder_init: return "encoder initialization";
      case initialization_failure_e::unsupported_format: return "unsupported format or settings";
      case initialization_failure_e::device_or_capture: return "device or capture setup";
      case initialization_failure_e::conversion: return "conversion setup";
      case initialization_failure_e::network: return "network";
      case initialization_failure_e::cancelled: return "cancelled";
      case initialization_failure_e::busy: return "runtime fence contention";
      case initialization_failure_e::unsafe_runtime: return "unsafe runtime";
    }
    return "unknown";
  }

  template<typename T>
  struct initialization_result_t {
    std::optional<T> value;
    initialization_failure_e failure = initialization_failure_e::none;
  };

  // Owned by one stream. A failed rollback ends that stream; the next stream
  // starts with native AMF again. Explicit implementations never switch.
  struct initialization_fallback_t {
    bool legacy = false;
    std::optional<initialization_failure_e> failed_rollback;

    template<typename Native, typename Legacy, typename Log>
    auto initialize(bool automatic, bool compatible, Native &&native, Legacy &&rollback, Log &&log) {
      using result_t = decltype(native());
      if (failed_rollback) return result_t {std::nullopt, *failed_rollback};
      if (legacy) return rollback();
      auto result = native();
      if (result.value || !automatic || !compatible ||
          (result.failure != initialization_failure_e::encoder_create &&
           result.failure != initialization_failure_e::encoder_init)) return result;
      // The native attempt publishes failure only after destroying its resources.
      // Set this before rollback so a failed rollback cannot loop back to native.
      legacy = true;
      log(result.failure);
      auto rolled_back = rollback();
      if (!rolled_back.value) failed_rollback = rolled_back.failure;
      return rolled_back;
    }
  };

  inline constexpr bool legacy_stream_format_compatible(
    int codec, int chroma, bool ten_bit, bool intra_refresh, bool native_options,
    int reference_frames = 0, bool reference_limit_supported = true) {
    return codec >= 0 && codec <= 2 && chroma == 0 &&
           !(codec == 0 && ten_bit) && !intra_refresh && !native_options &&
           (reference_frames == 0 || reference_limit_supported);
  }
}
