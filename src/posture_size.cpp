/**
 * @file src/posture_size.cpp
 * @brief Pure client-posture parsing and virtual display physical size policy.
 */
#include "posture_size.h"

#include <algorithm>
#include <cmath>

namespace posture {
  namespace {
    // Strict decimal integer: digits only, bounded length, inside [min, max].
    std::optional<int> parse_ranged_int(std::string_view value, int min, int max) {
      if (value.empty() || value.size() > 6) {
        return std::nullopt;
      }
      int parsed = 0;
      for (const char c : value) {
        if (c < '0' || c > '9') {
          return std::nullopt;
        }
        parsed = parsed * 10 + (c - '0');
      }
      if (parsed < min || parsed > max) {
        return std::nullopt;
      }
      return parsed;
    }
  }  // namespace

  std::string_view to_string(client_class_e client_class) {
    switch (client_class) {
      case client_class_e::desktop:
        return "desktop";
      case client_class_e::laptop:
        return "laptop";
      case client_class_e::tablet:
        return "tablet";
      case client_class_e::handheld:
        return "handheld";
      case client_class_e::phone:
        return "phone";
    }
    return "desktop";
  }

  std::optional<client_class_e> parse_posture(std::string_view value) {
    for (const auto client_class : {client_class_e::desktop, client_class_e::laptop, client_class_e::tablet, client_class_e::handheld, client_class_e::phone}) {
      if (value == to_string(client_class)) {
        return client_class;
      }
    }
    return std::nullopt;
  }

  std::optional<bool> parse_touch(std::string_view value) {
    if (value == "1") {
      return true;
    }
    if (value == "0") {
      return false;
    }
    return std::nullopt;
  }

  std::optional<int> parse_touch_diagonal_mm(std::string_view value) {
    return parse_ranged_int(value, min_touch_diagonal_mm, max_touch_diagonal_mm);
  }

  int sanitize_touch_diagonal_mm(int configured_mm) {
    if (configured_mm < min_touch_diagonal_mm || configured_mm > max_touch_diagonal_mm) {
      return default_touch_diagonal_mm;
    }
    return configured_mm;
  }

  bool hints_t::has_client_hints() const {
    return posture || touch;
  }

  hints_t parse_hints(std::string_view posture_value, std::string_view touch_value, bool paired_touch) {
    hints_t hints;
    hints.paired_touch = paired_touch;

    if (!posture_value.empty()) {
      hints.posture = parse_posture(posture_value);
      if (!hints.posture) {
        return hints;
      }
    }
    hints.touch = parse_touch(touch_value);
    return hints;
  }

  std::optional<client_class_e> effective_class(const hints_t &hints) {
    if (hints.posture) {
      switch (*hints.posture) {
        case client_class_e::tablet:
        case client_class_e::handheld:
        case client_class_e::phone:
          return hints.posture;
        default:
          return std::nullopt;
      }
    }
    if ((hints.touch && *hints.touch) || hints.paired_touch) {
      return client_class_e::tablet;
    }
    return std::nullopt;
  }

  bool touch_capable(const hints_t &hints) {
    return effective_class(hints).has_value() || (hints.touch && *hints.touch) || hints.paired_touch;
  }

  std::optional<client_override_e> parse_client_override(std::string_view value) {
    for (const auto candidate : {client_override_e::automatic, client_override_e::desktop, client_override_e::tablet, client_override_e::handheld}) {
      if (value == to_string(candidate)) {
        return candidate;
      }
    }
    return std::nullopt;
  }

  std::string_view to_string(client_override_e value) {
    switch (value) {
      case client_override_e::automatic:
        return "auto";
      case client_override_e::desktop:
        return "desktop";
      case client_override_e::tablet:
        return "tablet";
      case client_override_e::handheld:
        return "handheld";
    }
    return "auto";
  }

  hints_t apply_client_override(hints_t hints, client_override_e value) {
    switch (value) {
      case client_override_e::automatic:
        break;
      case client_override_e::tablet:
        hints.posture = client_class_e::tablet;
        hints.touch = true;
        break;
      case client_override_e::handheld:
        hints.posture = client_class_e::handheld;
        hints.touch = true;
        break;
      case client_override_e::desktop:
        hints.posture = touch_capable(hints) ? client_class_e::laptop : client_class_e::desktop;
        break;
    }
    return hints;
  }

  std::optional<handheld_policy_e> parse_handheld_policy(std::string_view value) {
    for (const auto candidate : {handheld_policy_e::tablet, handheld_policy_e::xbox_fullscreen, handheld_policy_e::desktop}) {
      if (value == to_string(candidate)) {
        return candidate;
      }
    }
    return std::nullopt;
  }

  std::string_view to_string(host_mode_e mode) {
    switch (mode) {
      case host_mode_e::desktop:
        return "desktop";
      case host_mode_e::tablet:
        return "tablet";
      case host_mode_e::handheld:
        return "handheld";
    }
    return "desktop";
  }

  std::string_view to_string(handheld_policy_e policy) {
    switch (policy) {
      case handheld_policy_e::tablet:
        return "tablet";
      case handheld_policy_e::xbox_fullscreen:
        return "xbox_fullscreen";
      case handheld_policy_e::desktop:
        return "desktop";
    }
    return "tablet";
  }

  host_mode_e host_mode(const hints_t &hints, handheld_policy_e policy) {
    const auto client_class = effective_class(hints);
    if (!client_class) {
      return host_mode_e::desktop;
    }
    if (*client_class == client_class_e::handheld) {
      switch (policy) {
        case handheld_policy_e::xbox_fullscreen:
          return host_mode_e::handheld;
        case handheld_policy_e::desktop:
          return host_mode_e::desktop;
        case handheld_policy_e::tablet:
          break;
      }
    }
    return host_mode_e::tablet;
  }

  std::optional<physical_size_t> derive_physical_size(const hints_t &hints, int width_px, int height_px, int diagonal_mm) {
    if (!touch_capable(hints)) {
      return std::nullopt;
    }

    const double diagonal = sanitize_touch_diagonal_mm(diagonal_mm);

    // Square pixels: the stream aspect only splits the fixed diagonal. 16:9 when the stream size is unknown.
    const bool have_pixels = width_px > 0 && height_px > 0;
    const double aspect_width = have_pixels ? width_px : 16;
    const double aspect_height = have_pixels ? height_px : 9;
    const double aspect_diagonal = std::hypot(aspect_width, aspect_height);

    const auto side_mm = [&](double aspect_side) {
      const double side = std::round(diagonal * aspect_side / aspect_diagonal);
      return static_cast<std::uint32_t>(std::max(side, static_cast<double>(min_side_mm)));
    };
    return physical_size_t {side_mm(aspect_width), side_mm(aspect_height)};
  }

  std::uint32_t size_diagonal_mm(const physical_size_t &size) {
    return static_cast<std::uint32_t>(std::lround(std::hypot(static_cast<double>(size.width_mm), static_cast<double>(size.height_mm))));
  }

  std::uint32_t edid_size_cm(std::uint32_t millimetres) {
    return std::clamp((millimetres + 5u) / 10u, 1u, 255u);
  }
}  // namespace posture
