/**
 * @file src/platform/linux/kmsgrab_hdr.h
 * @brief Suppress KMS HDR reinitialization only for validated equivalent blobs.
 */
#pragma once

#include "../audio.h"
#include "hdr_policy.h"

#include <cstring>
#include <drm_mode.h>
#include <limits>
#include <memory>
#include <mutex>
#include <xf86drmMode.h>

namespace platf::kms::hdr {

  struct state_t {
    bool present {};
    std::uint8_t eotf {};
    std::array<linux_hdr::chromaticity_t, 3> display_primaries {};
    linux_hdr::chromaticity_t white_point {};
    std::uint16_t max_display_luminance {};
    std::uint16_t min_display_luminance {};
    std::uint16_t max_content_light_level {};
    std::uint16_t max_frame_average_light_level {};

    bool operator==(const state_t &) const = default;

    [[nodiscard]] bool is_hdr() const {
      return present && linux_hdr::is_hdr10_eotf(eotf);
    }
  };

  /** Compare what KMS exposes to encoder selection, preserving raw state identity. */
  [[nodiscard]] inline bool same_encoder_signaling(const state_t &left, const state_t &right) {
    // is_hdr() rejects absent, SDR and unsupported-EOTF metadata. In those
    // states get_hdr_metadata() supplies nothing to the encoder, so changes
    // in otherwise parsed fields do not require another encoder generation.
    if (!left.is_hdr() && !right.is_hdr()) {
      return true;
    }
    return left == right;
  }

  /** Copy only validated fields; neither blob lifetime nor padding is semantic. */
  [[nodiscard]] inline std::optional<state_t> parse_blob(const void *data, std::size_t length) {
    constexpr auto info_offset = offsetof(hdr_output_metadata, hdmi_metadata_type1);
    if (!data || length < info_offset + sizeof(hdr_metadata_infoframe)) {
      return std::nullopt;
    }
    std::uint32_t metadata_type;
    hdr_metadata_infoframe info;
    std::memcpy(&metadata_type, data, sizeof(metadata_type));
    std::memcpy(&info, static_cast<const std::byte *>(data) + info_offset, sizeof(info));
    if (metadata_type != 0 || info.metadata_type != 0) {
      return std::nullopt;
    }
    state_t state {
      .present = true,
      .eotf = info.eotf,
      .white_point = {info.white_point.x, info.white_point.y},
      .max_display_luminance = info.max_display_mastering_luminance,
      .min_display_luminance = info.min_display_mastering_luminance,
      .max_content_light_level = info.max_cll,
      .max_frame_average_light_level = info.max_fall,
    };
    for (std::size_t i = 0; i < state.display_primaries.size(); ++i) {
      state.display_primaries[i] = {info.display_primaries[i].x, info.display_primaries[i].y};
    }
    return state;
  }

  /** Copy the immutable DRM blob while its libdrm allocation is owned. */
  [[nodiscard]] inline std::optional<state_t> read_blob(int fd, std::optional<std::uint64_t> id) {
    if (id && *id == 0) {
      return state_t {};
    }
    if (!id || *id > std::numeric_limits<std::uint32_t>::max()) {
      return std::nullopt;
    }
    std::unique_ptr<drmModePropertyBlobRes, decltype(&drmModeFreePropertyBlob)> blob {
      drmModeGetPropertyBlob(fd, static_cast<std::uint32_t>(*id)),
      drmModeFreePropertyBlob
    };
    return blob ? parse_blob(blob->data, blob->length) : std::nullopt;
  }

  /** One observation shared by HDR selection, metadata and capture comparison. */
  class generation_t {
  public:
    /** Seed a new strict-SDR capture before its factory publishes it. */
    [[nodiscard]] bool initialize_sdr(const state_t &state) {
      const std::lock_guard lock {state_mutex};
      // Never replace a state already exposed to encoder getters. Later
      // generations must continue through the owned stage/drain/commit path.
      if (initialized || (state.present && state.eotf != 0)) {
        return false;
      }
      selected_state = std::make_shared<const state_t>(state);
      initialized = true;
      return true;
    }

    [[nodiscard]] std::shared_ptr<const state_t> selected(int fd, std::optional<std::uint64_t> initial_id) {
      const std::lock_guard lock {state_mutex};
      if (!initialized) {
        if (const auto state = read_blob(fd, initial_id)) {
          selected_state = std::make_shared<const state_t>(*state);
        }
        initialized = true;
      }
      return selected_state;
    }

    [[nodiscard]] capture_e refresh(int fd, std::optional<std::uint64_t> initial_id, std::optional<std::uint64_t> observed_id, bool require_sdr = false) {
      const auto baseline = selected(fd, initial_id);
      const auto current = read_blob(fd, observed_id);
      const std::lock_guard lock {state_mutex};
      pending_state.reset();
      // The strict physical-SDR backend must validate this exact observation,
      // not just an earlier property read. Never stage unknown or non-SDR
      // state for a later same-instance encoder generation under that policy.
      if (require_sdr && (!current || (current->present && current->eotf != 0))) {
        return capture_e::reinit;
      }
      // Failure to read a blob/property is unknown, not evidence of a change.
      // Keep the selected state and retry on the next capture observation.
      if (!current) {
        return capture_e::ok;
      }
      const bool changed = baseline ? !same_encoder_signaling(*baseline, *current) : current->is_hdr();
      if (!changed) {
        return capture_e::ok;
      }
      // Stage the successful observation without changing what the old encoder
      // sees. This state never leaves its owning capture instance/connector.
      pending_state = *current;
      pending_generation = metadata_generation;
      return capture_e::reinit;
    }

    /** Called only after all encoders from the previous generation are gone. */
    [[nodiscard]] bool commit_pending() {
      const std::lock_guard lock {state_mutex};
      if (!pending_state || pending_generation != metadata_generation) {
        return false;
      }
      selected_state = std::make_shared<const state_t>(*pending_state);
      pending_state.reset();
      initialized = true;
      ++metadata_generation;
      return true;
    }

  private:
    std::mutex state_mutex;
    bool initialized {};
    std::shared_ptr<const state_t> selected_state;
    std::optional<state_t> pending_state;
    std::uint64_t metadata_generation {};
    std::uint64_t pending_generation {};
  };

}  // namespace platf::kms::hdr
