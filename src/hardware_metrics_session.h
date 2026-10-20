/**
 * @file src/hardware_metrics_session.h
 * @brief Read-only exact paired-owner video observation.
 */
#pragma once
#include <string_view>

namespace hardware_metrics {
  inline bool running_video(bool running, bool input_only) {
    return running && !input_only;
  }

  template<class Sessions, class Owners, class VideoPredicate>
  bool has_running_video_for_client(const Sessions &sessions, const Owners &owners, std::string_view uuid, VideoPredicate video) {
    if (uuid.empty()) {
      return false;
    }
    for (const auto &session : sessions) {
      const auto owner = owners.find(session.get());
      if (owner != owners.end() && owner->second == uuid && video(*session)) {
        return true;
      }
    }
    return false;
  }
}  // namespace hardware_metrics
