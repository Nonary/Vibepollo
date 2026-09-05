/**
 * @file src/platform/linux/gamescopegrab.cpp
 * @brief Rootless capture of Gamescope's main output through PipeWire.
 *
 * Stock Gamescope v1 exposes one compositor-owned PipeWire node. It does not
 * provide mode control or an HDR capability contract. The separate
 * vibeshine_capture_v1 extension supplies a verified HDR10 capture profile.
 */
#include "gamescopegrab.h"

#include "gamescope_hdr_policy.h"
#include "gamescope_session.h"
#include "pipewire.cpp"
#include "src/config.h"
#include "wayland_roundtrip.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <gamescope-pipewire.h>
#include <memory>
#include <pipewire/pipewire.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <vibeshine-capture-v1.h>
#include <wayland-client.h>

using namespace std::literals;

namespace gamescope {
  class pipewire_node_t {
  public:
    pipewire_node_t() = default;
    pipewire_node_t(const pipewire_node_t &) = delete;
    pipewire_node_t &operator=(const pipewire_node_t &) = delete;

    ~pipewire_node_t() {
      if (hdr_global_) {
        vibeshine_capture_v1_destroy(hdr_global_);
      }
      if (pipewire_global_) {
        gamescope_pipewire_destroy(pipewire_global_);
      }
      for (auto *output : outputs_) {
        wl_output_destroy(output);
      }
      if (registry_) {
        wl_registry_destroy(registry_);
      }
      if (display_) {
        wl_display_disconnect(display_);
      }
    }

    int init(const std::string &display_name, const bool log_errors) {
      display_ = wl_display_connect(display_name.c_str());
      if (!display_) {
        if (log_errors) {
          BOOST_LOG(error) << "[gamescope] Could not connect to Wayland display '"sv << display_name << "'"sv;
        }
        return -1;
      }

      registry_ = wl_display_get_registry(display_);
      if (!registry_ || wl_registry_add_listener(registry_, &registry_listener_, this) < 0 || platf::wayland::roundtrip(display_, 1500ms) < 0) {
        if (log_errors) {
          BOOST_LOG(error) << "[gamescope] Failed to enumerate the Wayland registry"sv;
        }
        return -1;
      }

      // The v1 protocol guarantees stream_node after a roundtrip following
      // the bind. This also completes wl_output mode delivery.
      if (platf::wayland::roundtrip(display_, 1500ms) < 0 || !pipewire_global_ || !node_received_) {
        if (log_errors) {
          BOOST_LOG(error) << "[gamescope] gamescope_pipewire v1 did not advertise a PipeWire node"sv;
        }
        return -1;
      }
      return 0;
    }

    uint32_t node_id() const {
      return node_id_;
    }

    int width() const {
      return width_;
    }

    int height() const {
      return height_;
    }

    bool hdr_capable() const {
      return platf::gamescope_hdr::supported(hdr_profile_, hdr_node_, node_id_);
    }

  private:
    static void on_registry_global(void *data, wl_registry *registry, const uint32_t name, const char *interface, const uint32_t version) {
      auto *self = static_cast<pipewire_node_t *>(data);
      if (std::strcmp(interface, gamescope_pipewire_interface.name) == 0) {
        self->pipewire_global_ = static_cast<gamescope_pipewire *>(
          wl_registry_bind(registry, name, &gamescope_pipewire_interface, std::min(version, 1u))
        );
        gamescope_pipewire_add_listener(self->pipewire_global_, &pipewire_listener_, self);
      } else if (std::strcmp(interface, vibeshine_capture_v1_interface.name) == 0) {
        self->hdr_global_ = static_cast<vibeshine_capture_v1 *>(
          wl_registry_bind(registry, name, &vibeshine_capture_v1_interface, 1)
        );
        vibeshine_capture_v1_add_listener(self->hdr_global_, &hdr_listener_, self);
      } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
        auto *output = static_cast<wl_output *>(
          wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 4u))
        );
        self->outputs_.push_back(output);
        wl_output_add_listener(output, &output_listener_, self);
      }
    }

    static void on_registry_global_remove(void *, wl_registry *, uint32_t) {
    }

    static void on_stream_node(void *data, gamescope_pipewire *, const uint32_t node_id) {
      auto *self = static_cast<pipewire_node_t *>(data);
      self->node_id_ = node_id;
      self->node_received_ = node_id != PW_ID_ANY;
    }

    static void on_output_geometry(void *, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t, const char *, const char *, int32_t) {
    }

    static void on_output_mode(void *data, wl_output *, const uint32_t flags, const int32_t width, const int32_t height, int32_t) {
      if (!(flags & WL_OUTPUT_MODE_CURRENT) || width <= 0 || height <= 0) {
        return;
      }
      auto *self = static_cast<pipewire_node_t *>(data);
      if (static_cast<int64_t>(width) * height > static_cast<int64_t>(self->width_) * self->height_) {
        self->width_ = width;
        self->height_ = height;
      }
    }

    static void on_output_done(void *, wl_output *) {
    }

    static void on_output_scale(void *, wl_output *, int32_t) {
    }

    static void on_output_name(void *, wl_output *, const char *) {
    }

    static void on_output_description(void *, wl_output *, const char *) {
    }

    static constexpr wl_registry_listener registry_listener_ = {
      .global = on_registry_global,
      .global_remove = on_registry_global_remove,
    };

    static constexpr gamescope_pipewire_listener pipewire_listener_ = {
      .stream_node = on_stream_node,
    };

    static void on_hdr_profile(void *data, vibeshine_capture_v1 *, uint32_t node, uint32_t profile) {
      auto *self = static_cast<pipewire_node_t *>(data);
      self->hdr_node_ = node;
      self->hdr_profile_ = profile;
    }

    static constexpr vibeshine_capture_v1_listener hdr_listener_ {
      .hdr10_profile = on_hdr_profile,
    };

    static constexpr wl_output_listener output_listener_ = {
      .geometry = on_output_geometry,
      .mode = on_output_mode,
      .done = on_output_done,
      .scale = on_output_scale,
      .name = on_output_name,
      .description = on_output_description,
    };

    wl_display *display_ = nullptr;
    wl_registry *registry_ = nullptr;
    gamescope_pipewire *pipewire_global_ = nullptr;
    vibeshine_capture_v1 *hdr_global_ = nullptr;
    uint32_t hdr_node_ = PW_ID_ANY;
    uint32_t hdr_profile_ = 0;
    std::vector<wl_output *> outputs_;
    uint32_t node_id_ = PW_ID_ANY;
    bool node_received_ = false;
    int width_ = 0;
    int height_ = 0;
  };

  class display_t: public pipewire::pipewire_display_t {
  public:
    display_t(std::string wayland_display, const int requested_width, const int requested_height, bool hdr_requested):
        wayland_display_ {std::move(wayland_display)},
        requested_width_ {requested_width},
        requested_height_ {requested_height},
        hdr_requested_ {hdr_requested} {
      pipewire.set_gamescope_requested_size(true);
      pipewire.set_negotiate_maxframerate(false);
      // HDR is opt-in and requires the separate extension contract. Never
      // allow a failed HDR negotiation to silently choose an 8-bit buffer.
      pipewire.set_force_sdr_formats(!hdr_requested_);
      pipewire.set_force_hdr10_formats(hdr_requested_);
    }

    int configure_stream(const std::string &display_name, int &out_pipewire_fd, uint32_t &out_pipewire_node, uint64_t &out_pipewire_objectserial) override {
      if (display_name != "gamescope" && !display_name.empty()) {
        BOOST_LOG(error) << "[gamescope] Unknown display name '"sv << display_name << "'"sv;
        return -1;
      }

      node_ = std::make_unique<pipewire_node_t>();
      if (node_->init(wayland_display_, true) < 0) {
        return -1;
      }
      if (hdr_requested_ && !node_->hdr_capable()) {
        BOOST_LOG(error) << "[gamescope] HDR requires the version-matched Vibeshine capture extension; this compositor exposes SDR only."sv;
        return -1;
      }

      out_pipewire_fd = -1;
      out_pipewire_node = node_->node_id();
      out_pipewire_objectserial = SPA_ID_INVALID;
      offset_x = 0;
      offset_y = 0;
      const int native_width = node_->width();
      const int native_height = node_->height();
      width = requested_width_ > 0 ? requested_width_ : native_width;
      height = requested_height_ > 0 ? requested_height_ : native_height;
      if (native_width > 0 && native_height > 0 && width > 0 && height > 0) {
        // Match Gamescope's aspect-preserving, even-sized capture bounds.
        // Wait for this size, rather than mistaking its first native-size
        // format event for completion of the asynchronous downscale request.
        if (static_cast<int64_t>(width) * native_height <= static_cast<int64_t>(height) * native_width) {
          expected_width_ = std::min(width, native_width);
          expected_height_ = (static_cast<int64_t>(expected_width_) * native_height + native_width - 1) / native_width;
        } else {
          expected_height_ = std::min(height, native_height);
          expected_width_ = (static_cast<int64_t>(expected_height_) * native_width + native_height - 1) / native_height;
        }
        expected_width_ = (expected_width_ + 1) & ~1;
        expected_height_ = (expected_height_ + 1) & ~1;
      }
      if (native_width > 0 && native_height > 0 && (width > native_width || height > native_height)) {
        BOOST_LOG(warning) << "[gamescope] Stock Gamescope cannot render a capture larger than its main output (requested "sv
                           << width << 'x' << height << ", output "sv << native_width << 'x' << native_height
                           << "); PipeWire negotiation will use the compositor-supported size"sv;
      } else {
        BOOST_LOG(info) << "[gamescope] Requesting compositor-side capture bounds "sv << width << 'x' << height
                        << " without changing the physical output mode (stock Gamescope preserves output aspect ratio)"sv;
      }
      logical_width = width;
      logical_height = height;
      env_width = width;
      env_height = height;
      env_logical_width = width;
      env_logical_height = height;
      return width > 0 && height > 0 ? 0 : -1;
    }

    void verify_and_update_display_parameters() override {
      // Gamescope is a nested compositor with one capture scene. Querying the
      // parent session's global wl::monitors() here would produce the wrong
      // coordinate space for absolute input.
      offset_x = 0;
      offset_y = 0;
      logical_width = width;
      logical_height = height;
      env_width = width;
      env_height = height;
      env_logical_width = width;
      env_logical_height = height;
    }

    bool is_hdr() override {
      return shared_state && node_ && platf::gamescope_hdr::negotiated(hdr_requested_, node_->hdr_capable(), shared_state->pixel_format.load() == SPA_VIDEO_FORMAT_xBGR_210LE, shared_state->color_primaries.load() == SPA_VIDEO_COLOR_PRIMARIES_BT2020, shared_state->transfer_function.load() == SPA_VIDEO_TRANSFER_SMPTE2084, shared_state->color_range.load() == SPA_VIDEO_COLOR_RANGE_0_255 && shared_state->color_matrix.load() == SPA_VIDEO_COLOR_MATRIX_RGB);
    }

    bool is_codec_supported(std::string_view, const video::config_t &config) override {
      return !config.dynamicRange || config.force_sdr || (node_ && node_->hdr_capable());
    }

    bool get_hdr_metadata(SS_HDR_METADATA &metadata) override {
      if (!is_hdr()) {
        return false;
      }
      // The extension defines a virtual BT.2020/PQ reference volume. These
      // are its rendering bounds, not invented physical-panel measurements.
      // Content-light levels stay unknown rather than borrowing another output.
      metadata = {};
      metadata.displayPrimaries[0] = {35400, 14600};
      metadata.displayPrimaries[1] = {8500, 39850};
      metadata.displayPrimaries[2] = {6550, 2300};
      metadata.whitePoint = {15635, 16450};
      metadata.maxDisplayLuminance = 10000;
      return true;
    }

  protected:
    bool capture_format_valid() override {
      return !hdr_requested_ || is_hdr();
    }

    bool negotiated_size_ready(int negotiated_width, int negotiated_height) const override {
      return expected_width_ <= 0 || expected_height_ <= 0 ||
             (negotiated_width == expected_width_ && negotiated_height == expected_height_);
    }

    std::chrono::milliseconds negotiated_size_settle_time() const override {
      // Gamescope initially advertises its output size, reads the consumer's
      // private requested-size property, then updates the producer format.
      // Avoid committing encoder dimensions from that transient first event.
      return 150ms;
    }

    const char *wayland_display_name() const override {
      return wayland_display_.c_str();
    }

  private:
    std::string wayland_display_;
    int requested_width_;
    int requested_height_;
    int expected_width_ = 0;
    int expected_height_ = 0;
    std::unique_ptr<pipewire_node_t> node_;
    bool hdr_requested_ = false;
  };
}  // namespace gamescope

namespace platf {
  bool gamescope_available() {
    const auto wayland_display = gamescope_session::discover_wayland_display();
    if (!wayland_display) {
      return false;
    }
    gamescope::pipewire_node_t node;
    return node.init(*wayland_display, false) == 0;
  }

  std::vector<std::string> gamescope_display_names() {
    if (!gamescope_available()) {
      return {};
    }
    return {"gamescope"};
  }

  std::shared_ptr<display_t> gamescope_display(const mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (!pipewire::pipewire_display_t::init_pipewire_and_check_hwdevice_type(hwdevice_type)) {
      BOOST_LOG(error) << "[gamescope] Unsupported capture memory type"sv;
      return nullptr;
    }
    const bool hdr_requested = config.dynamicRange && !config.force_sdr;
    if (hdr_requested && hwdevice_type != mem_type_e::vaapi && hwdevice_type != mem_type_e::vulkan) {
      BOOST_LOG(error) << "[gamescope] HDR capture requires the VAAPI or Vulkan DMA-BUF path."sv;
      return nullptr;
    }
    const auto wayland_display = gamescope_session::discover_wayland_display();
    if (!wayland_display) {
      BOOST_LOG(error) << "[gamescope] GAMESCOPE_WAYLAND_DISPLAY was not found for the current session"sv;
      return nullptr;
    }

    auto display = std::make_shared<gamescope::display_t>(*wayland_display, config.width, config.height, hdr_requested);
    if (display->init(hwdevice_type, display_name, config) < 0) {
      return nullptr;
    }
    if (hdr_requested && !display->is_hdr()) {
      BOOST_LOG(error) << "[gamescope] HDR negotiation did not produce 10-bit BT.2020/PQ full-range pixels."sv;
      return nullptr;
    }
    BOOST_LOG(info) << (hdr_requested ? "[gamescope] Using verified HDR10 capture extension"sv : "[gamescope] Using SDR capture"sv);
    return display;
  }
}  // namespace platf
