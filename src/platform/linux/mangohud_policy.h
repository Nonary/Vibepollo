/**
 * @file src/platform/linux/mangohud_policy.h
 * @brief Pure policy helpers for the Linux MangoHUD frame limiter.
 */
#pragma once

#include "src/framegen_policy.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace platf::mangohud {

  inline constexpr std::string_view preload_library = "/usr/$LIB/mangohud/libMangoHud_shim.so";

  struct launch_policy_t {
    bool enabled = false;
    std::uint32_t limit_millihz = 0;
    std::string limit;
  };

  inline std::string normalize_provider(std::string_view provider) {
    std::string normalized;
    normalized.reserve(provider.size());
    for (const char ch : provider) {
      if (ch == '-' || ch == '_' || std::isspace(static_cast<unsigned char>(ch))) {
        continue;
      }
      normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return normalized;
  }

  inline bool provider_selected(std::string_view provider) {
    const auto normalized = normalize_provider(provider);
    return normalized.empty() || normalized == "auto" || normalized == "mangohud";
  }

  inline std::string with_preload(std::string_view current) {
    std::size_t start = 0;
    while (start <= current.size()) {
      const auto end = current.find(':', start);
      const auto item = current.substr(start, end == std::string_view::npos ? current.size() - start : end - start);
      if (item == preload_library) {
        return std::string(current);
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    if (current.empty()) {
      return std::string(preload_library);
    }
    return std::string(current) + ":" + std::string(preload_library);
  }

  inline std::string without_preload(std::string_view current) {
    std::string result;
    std::size_t start = 0;
    while (start <= current.size()) {
      const auto end = current.find(':', start);
      const auto item = current.substr(start, end == std::string_view::npos ? current.size() - start : end - start);
      if (!item.empty() && item != preload_library) {
        if (!result.empty()) {
          result.push_back(':');
        }
        result.append(item);
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return result;
  }

  inline std::string format_limit(std::uint32_t millihz) {
    const auto whole = millihz / 1000;
    auto fractional = millihz % 1000;
    if (fractional == 0) {
      return std::to_string(whole);
    }

    std::string result = std::to_string(whole) + ".";
    result.push_back(static_cast<char>('0' + fractional / 100));
    fractional %= 100;
    result.push_back(static_cast<char>('0' + fractional / 10));
    result.push_back(static_cast<char>('0' + fractional % 10));
    while (result.back() == '0') {
      result.pop_back();
    }
    return result;
  }

  inline launch_policy_t make_launch_policy(
    std::string_view provider,
    bool limiter_enabled,
    bool automatic_virtual_limiter,
    const framegen::stream_start_policy_t &stream_policy,
    std::uint32_t configured_limit_millihz
  ) {
    const bool policy_limiter =
      stream_policy.uses_virtual_display && automatic_virtual_limiter;
    if (!provider_selected(provider) || (!limiter_enabled && !policy_limiter)) {
      return {};
    }

    std::uint32_t limit_millihz = stream_policy.frame_limit_millihz > 0 ?
                                      stream_policy.frame_limit_millihz :
                                      (stream_policy.fps > 0 ?
                                         framegen::saturating_refresh_millihz(
                                           static_cast<std::uint32_t>(stream_policy.fps), 1000
                                         ) :
                                         0);
    if (stream_policy.lossless_rtss_limit && *stream_policy.lossless_rtss_limit > 0) {
      limit_millihz = framegen::saturating_refresh_millihz(
        static_cast<std::uint32_t>(*stream_policy.lossless_rtss_limit), 1000
      );
    }
    if (configured_limit_millihz > 0) {
      limit_millihz = configured_limit_millihz;
    }
    limit_millihz = std::min<std::uint32_t>(limit_millihz, 1'000'000);
    if (limit_millihz == 0) {
      return {};
    }

    return {
      .enabled = true,
      .limit_millihz = limit_millihz,
      .limit = format_limit(limit_millihz),
    };
  }

}  // namespace platf::mangohud
