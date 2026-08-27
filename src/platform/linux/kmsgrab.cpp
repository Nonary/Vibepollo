/**
 * @file src/platform/linux/kmsgrab.cpp
 * @brief Definitions for KMS screen capture.
 */
// standard includes
#include <array>
#include <errno.h>
#include <fcntl.h>
#include <filesystem>
#include <thread>
#include <unistd.h>

// platform includes
#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#include <sys/capability.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <vibeshine_drm_uapi.h>

// local includes
#include "cuda.h"
#include "graphics.h"
#include "hdr_policy.h"
#include "kmsgrab_pacing.h"
#include "kmsgrab_selection.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/round_robin.h"
#include "src/utility.h"
#include "src/video.h"
#include "vaapi.h"
#include "vulkan_encode.h"
#include "wayland.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace platf {

  namespace kms {

#define DRM_IOCTL_VIBESHINE_WAIT_PRESENT \
  DRM_IOWR(DRM_COMMAND_BASE + DRM_VIBESHINE_WAIT_PRESENT, struct vibeshine_drm_wait_present)
#define DRM_IOCTL_VIBESHINE_GET_FRAME \
  DRM_IOWR(DRM_COMMAND_BASE + DRM_VIBESHINE_GET_FRAME, struct vibeshine_drm_frame)

    static_assert(sizeof(vibeshine_drm_wait_present) == 48);
    static_assert(sizeof(vibeshine_drm_frame) == 152);

    constexpr auto PRESENT_WAIT_IDLE_TIMEOUT = std::chrono::milliseconds(16);

    class cap_sys_admin {
    public:
      cap_sys_admin() {
        caps = cap_get_proc();

        cap_value_t sys_admin = CAP_SYS_ADMIN;
        if (cap_set_flag(caps, CAP_EFFECTIVE, 1, &sys_admin, CAP_SET) || cap_set_proc(caps)) {
          BOOST_LOG(error) << "Failed to gain CAP_SYS_ADMIN";
        }
      }

      ~cap_sys_admin() {
        cap_value_t sys_admin = CAP_SYS_ADMIN;
        if (cap_set_flag(caps, CAP_EFFECTIVE, 1, &sys_admin, CAP_CLEAR) || cap_set_proc(caps)) {
          BOOST_LOG(error) << "Failed to drop CAP_SYS_ADMIN";
        }
        cap_free(caps);
      }

      cap_t caps;
    };

    class wrapper_fb {
    public:
      wrapper_fb(uint32_t card_fd, drmModeFB *fb):
          card_fd {card_fd},
          fb {fb},
          fb_id {fb->fb_id},
          width {fb->width},
          height {fb->height} {
        pixel_format = DRM_FORMAT_XRGB8888;
        modifier = DRM_FORMAT_MOD_INVALID;
        std::fill_n(handles, 4, 0);
        std::fill_n(pitches, 4, 0);
        std::fill_n(offsets, 4, 0);
        handles[0] = fb->handle;
        pitches[0] = fb->pitch;
      }

      wrapper_fb(uint32_t card_fd, drmModeFB2 *fb2):
          card_fd {card_fd},
          fb2 {fb2},
          fb_id {fb2->fb_id},
          width {fb2->width},
          height {fb2->height} {
        pixel_format = fb2->pixel_format;
        modifier = (fb2->flags & DRM_MODE_FB_MODIFIERS) ? fb2->modifier : DRM_FORMAT_MOD_INVALID;

        memcpy(handles, fb2->handles, sizeof(handles));
        memcpy(pitches, fb2->pitches, sizeof(pitches));
        memcpy(offsets, fb2->offsets, sizeof(offsets));
      }

      ~wrapper_fb() {
        std::ranges::for_each(handles, [&](auto &handle) {
          if (handle) {
            struct drm_gem_close close_args = {};
            close_args.handle = handle;

            drmIoctl(card_fd, DRM_IOCTL_GEM_CLOSE, &close_args);
          }
        });

        if (fb) {
          drmModeFreeFB(fb);
        } else if (fb2) {
          drmModeFreeFB2(fb2);
        }
      }

      uint32_t card_fd;
      drmModeFB *fb = nullptr;
      drmModeFB2 *fb2 = nullptr;
      uint32_t fb_id;
      uint32_t width;
      uint32_t height;
      uint32_t pixel_format;
      uint64_t modifier;
      uint32_t handles[4];
      uint32_t pitches[4];
      uint32_t offsets[4];
    };

    using plane_res_t = util::safe_ptr<drmModePlaneRes, drmModeFreePlaneResources>;
    using encoder_t = util::safe_ptr<drmModeEncoder, drmModeFreeEncoder>;
    using res_t = util::safe_ptr<drmModeRes, drmModeFreeResources>;
    using plane_t = util::safe_ptr<drmModePlane, drmModeFreePlane>;
    using fb_t = std::unique_ptr<wrapper_fb>;
    using crtc_t = util::safe_ptr<drmModeCrtc, drmModeFreeCrtc>;
    using obj_prop_t = util::safe_ptr<drmModeObjectProperties, drmModeFreeObjectProperties>;
    using prop_t = util::safe_ptr<drmModePropertyRes, drmModeFreeProperty>;
    using prop_blob_t = util::safe_ptr<drmModePropertyBlobRes, drmModeFreePropertyBlob>;
    using version_t = util::safe_ptr<drmVersion, drmFreeVersion>;

    static int env_width;
    static int env_height;

    static int env_logical_width;
    static int env_logical_height;

    std::string_view plane_type(std::uint64_t val) {
      switch (val) {
        case DRM_PLANE_TYPE_OVERLAY:
          return "DRM_PLANE_TYPE_OVERLAY"sv;
        case DRM_PLANE_TYPE_PRIMARY:
          return "DRM_PLANE_TYPE_PRIMARY"sv;
        case DRM_PLANE_TYPE_CURSOR:
          return "DRM_PLANE_TYPE_CURSOR"sv;
      }

      return "UNKNOWN"sv;
    }

    struct connector_t {
      // For example: HDMI-A or HDMI
      std::uint32_t type;

      // Equals zero if not applicable
      std::uint32_t crtc_id;

      // For example HDMI-A-{index} or HDMI-{index}
      std::uint32_t index;

      // Kernel/Wayland connector name, for example HDMI-A-1 or Virtual-1
      std::string name;

      // ID of the connector
      std::uint32_t connector_id;

      bool connected;
    };

    struct monitor_t {
      // Connector attributes
      std::uint32_t type;
      std::uint32_t index;
      std::string connector_name;

      // Monitor index in the global list
      std::optional<std::uint32_t> monitor_index;

      platf::touch_port_t viewport;
    };

    struct card_descriptor_t {
      std::string path;

      std::map<std::uint32_t, monitor_t> crtc_to_monitor;
    };

    static std::vector<card_descriptor_t> card_descriptors;
    static std::vector<selection::named_monitor_t> named_monitors;

    static std::uint32_t from_view(const std::string_view &string) {
#define _CONVERT(x, y) \
  if (string == x) \
  return DRM_MODE_CONNECTOR_##y

      // This list was created from the following sources:
      // https://gitlab.freedesktop.org/mesa/drm/-/blob/main/xf86drmMode.c (drmModeGetConnectorTypeName)
      // https://gitlab.freedesktop.org/wayland/weston/-/blob/e74f2897b9408b6356a555a0ce59146836307ff5/libweston/backend-drm/drm.c#L1458-1477
      // https://github.com/GNOME/mutter/blob/65d481594227ea7188c0416e8e00b57caeea214f/src/backends/meta-monitor-manager.c#L1618-L1639
      _CONVERT("VGA"sv, VGA);
      _CONVERT("DVII"sv, DVII);
      _CONVERT("DVI-I"sv, DVII);
      _CONVERT("DVID"sv, DVID);
      _CONVERT("DVI-D"sv, DVID);
      _CONVERT("DVIA"sv, DVIA);
      _CONVERT("DVI-A"sv, DVIA);
      _CONVERT("Composite"sv, Composite);
      _CONVERT("SVIDEO"sv, SVIDEO);
      _CONVERT("S-Video"sv, SVIDEO);
      _CONVERT("LVDS"sv, LVDS);
      _CONVERT("Component"sv, Component);
      _CONVERT("9PinDIN"sv, 9PinDIN);
      _CONVERT("DIN"sv, 9PinDIN);
      _CONVERT("DisplayPort"sv, DisplayPort);
      _CONVERT("DP"sv, DisplayPort);
      _CONVERT("HDMIA"sv, HDMIA);
      _CONVERT("HDMI-A"sv, HDMIA);
      _CONVERT("HDMI"sv, HDMIA);
      _CONVERT("HDMIB"sv, HDMIB);
      _CONVERT("HDMI-B"sv, HDMIB);
      _CONVERT("TV"sv, TV);
      _CONVERT("eDP"sv, eDP);
      _CONVERT("VIRTUAL"sv, VIRTUAL);
      _CONVERT("Virtual"sv, VIRTUAL);
      _CONVERT("DSI"sv, DSI);
      _CONVERT("DPI"sv, DPI);
      _CONVERT("WRITEBACK"sv, WRITEBACK);
      _CONVERT("Writeback"sv, WRITEBACK);
      _CONVERT("SPI"sv, SPI);
#ifdef DRM_MODE_CONNECTOR_USB
      _CONVERT("USB"sv, USB);
#endif

      // If the string starts with "Unknown", it may have the raw type
      // value appended to the string. Let's try to read it.
      if (string.find("Unknown"sv) == 0) {
        std::uint32_t type;
        std::string null_terminated_string {string};
        if (std::sscanf(null_terminated_string.c_str(), "Unknown%u", &type) == 1) {
          return type;
        }
      }

      BOOST_LOG(error) << "Unknown Monitor connector type ["sv << string << "]: Please report this to the GitHub issue tracker"sv;
      return DRM_MODE_CONNECTOR_Unknown;
    }

    class plane_it_t: public round_robin_util::it_wrap_t<plane_t::element_type, plane_it_t> {
    public:
      plane_it_t(int fd, std::uint32_t *plane_p, std::uint32_t *end):
          fd {fd},
          plane_p {plane_p},
          end {end} {
        load_next_valid_plane();
      }

      plane_it_t(int fd, std::uint32_t *end):
          fd {fd},
          plane_p {end},
          end {end} {
      }

      void load_next_valid_plane() {
        this->plane.reset();

        for (; plane_p != end; ++plane_p) {
          plane_t plane = drmModeGetPlane(fd, *plane_p);
          if (!plane) {
            BOOST_LOG(error) << "Couldn't get drm plane ["sv << (end - plane_p) << "]: "sv << strerror(errno);
            continue;
          }

          this->plane = util::make_shared<plane_t>(plane.release());
          break;
        }
      }

      void inc() {
        ++plane_p;
        load_next_valid_plane();
      }

      bool eq(const plane_it_t &other) const {
        return plane_p == other.plane_p;
      }

      plane_t::pointer get() {
        return plane.get();
      }

      int fd;
      std::uint32_t *plane_p;
      std::uint32_t *end;

      util::shared_t<plane_t> plane;
    };

    struct cursor_t {
      // Public properties used during blending
      bool visible = false;
      std::int32_t x;
      std::int32_t y;
      std::uint32_t dst_w;
      std::uint32_t dst_h;
      std::uint32_t src_w;
      std::uint32_t src_h;
      std::vector<std::uint8_t> pixels;
      unsigned long serial;

      // Private properties used for tracking cursor changes
      std::uint64_t prop_src_x;
      std::uint64_t prop_src_y;
      std::uint64_t prop_src_w;
      std::uint64_t prop_src_h;
      std::uint32_t fb_id;
    };

    class card_t {
    public:
      using connector_interal_t = util::safe_ptr<drmModeConnector, drmModeFreeConnector>;

      int init(const char *path) {
        cap_sys_admin admin;
        fd.el = open(path, O_RDWR);

        if (fd.el < 0) {
          BOOST_LOG(error) << "Couldn't open: "sv << path << ": "sv << strerror(errno);
          return -1;
        }

        version_t ver {drmGetVersion(fd.el)};
        auto validated_driver_name = selection::normalize_driver_name(
          ver ? ver->name : nullptr,
          ver ? static_cast<std::size_t>(ver->name_len) : 0
        );
        if (!validated_driver_name) {
          BOOST_LOG(error) << "Couldn't obtain a valid DRM driver identity for: "sv << path;
          return -1;
        }
        driver_name = std::move(*validated_driver_name);
        BOOST_LOG(info) << path << " -> "sv << driver_name << " "sv
                        << ver->version_major << '.' << ver->version_minor << '.' << ver->version_patchlevel;

        // Open the render node for this card to share with libva.
        // If it fails, we'll just share the primary node instead.
        char *rendernode_path = drmGetRenderDeviceNameFromFd(fd.el);
        if (rendernode_path) {
          BOOST_LOG(debug) << "Opening render node: "sv << rendernode_path;
          render_fd.el = open(rendernode_path, O_RDWR);
          if (render_fd.el < 0) {
            BOOST_LOG(warning) << "Couldn't open render node: "sv << rendernode_path << ": "sv << strerror(errno);
            render_fd.el = dup(fd.el);
          }
          free(rendernode_path);
        } else {
          BOOST_LOG(warning) << "No render device name for: "sv << path;
          render_fd.el = dup(fd.el);
        }

        if (drmSetClientCap(fd.el, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
          BOOST_LOG(error) << "GPU driver doesn't support universal planes: "sv << path;
          return -1;
        }

        if (drmSetClientCap(fd.el, DRM_CLIENT_CAP_ATOMIC, 1)) {
          BOOST_LOG(warning) << "GPU driver doesn't support atomic mode-setting: "sv << path;
#if defined(SUNSHINE_BUILD_X11)
          // We won't be able to capture the mouse cursor with KMS on non-atomic drivers,
          // so fall back to X11 if it's available and the user didn't explicitly force KMS.
          if (window_system == window_system_e::X11 && config::video.capture != "kms") {
            BOOST_LOG(info) << "Avoiding KMS capture under X11 due to lack of atomic mode-setting"sv;
            return -1;
          }
#endif
          BOOST_LOG(warning) << "Cursor capture may fail without atomic mode-setting support!"sv;
        }

        plane_res.reset(drmModeGetPlaneResources(fd.el));
        if (!plane_res) {
          BOOST_LOG(error) << "Couldn't get drm plane resources"sv;
          return -1;
        }

        return 0;
      }

      fb_t fb(plane_t::pointer plane) {
        cap_sys_admin admin;

        auto fb2 = drmModeGetFB2(fd.el, plane->fb_id);
        if (fb2) {
          return std::make_unique<wrapper_fb>(fd.el, fb2);
        }

        auto fb = drmModeGetFB(fd.el, plane->fb_id);
        if (fb) {
          return std::make_unique<wrapper_fb>(fd.el, fb);
        }

        return nullptr;
      }

      crtc_t crtc(std::uint32_t id) {
        return drmModeGetCrtc(fd.el, id);
      }

      encoder_t encoder(std::uint32_t id) {
        return drmModeGetEncoder(fd.el, id);
      }

      res_t res() {
        return drmModeGetResources(fd.el);
      }

      bool is_nvidia() {
        return selection::driver_is_nvidia(driver_name);
      }

      bool supports_cuda_import() {
        return selection::driver_supports_cuda_import(driver_name);
      }

      bool requires_direct_import() {
        return selection::driver_requires_direct_import(driver_name);
      }

      bool requires_presentation_events() {
        return selection::driver_requires_presentation_events(driver_name);
      }

      bool is_cursor(std::uint32_t plane_id) {
        auto props = plane_props(plane_id);
        for (auto &[prop, val] : props) {
          if (prop->name == "type"sv) {
            if (val == DRM_PLANE_TYPE_CURSOR) {
              return true;
            } else {
              return false;
            }
          }
        }

        return false;
      }

      std::optional<std::uint64_t> prop_value_by_name(const std::vector<std::pair<prop_t, std::uint64_t>> &props, std::string_view name) {
        for (auto &[prop, val] : props) {
          if (prop->name == name) {
            return val;
          }
        }
        return std::nullopt;
      }

      std::uint32_t get_panel_orientation(std::uint32_t plane_id) {
        auto props = plane_props(plane_id);
        auto value = prop_value_by_name(props, "rotation"sv);
        if (value) {
          return *value;
        }

        BOOST_LOG(error) << "Failed to determine panel orientation, defaulting to landscape.";
        return DRM_MODE_ROTATE_0;
      }

      int get_crtc_index_by_id(std::uint32_t crtc_id) {
        auto resources = res();
        for (int i = 0; i < resources->count_crtcs; i++) {
          if (resources->crtcs[i] == crtc_id) {
            return i;
          }
        }
        return -1;
      }

      connector_interal_t connector(std::uint32_t id) {
        return drmModeGetConnector(fd.el, id);
      }

      std::vector<connector_t> monitors() {
        auto resources = res();
        if (!resources) {
          BOOST_LOG(error) << "Couldn't get connector resources"sv;
          return {};
        }

        std::vector<connector_t> monitors;
        std::for_each_n(resources->connectors, resources->count_connectors, [this, &monitors](std::uint32_t id) {
          auto conn = connector(id);
          if (!conn) {
            BOOST_LOG(error) << "Couldn't get drm connector ["sv << id << "]: "sv << strerror(errno);
            return;
          }

          std::uint32_t crtc_id = 0;

          if (conn->encoder_id) {
            auto enc = encoder(conn->encoder_id);
            if (enc) {
              crtc_id = enc->crtc_id;
            }
          }

          const auto *type_name = drmModeGetConnectorTypeName(conn->connector_type);
          std::string connector_name = type_name ? type_name : "Unknown";
          connector_name += '-';
          connector_name += std::to_string(conn->connector_type_id);

          monitors.emplace_back(connector_t {
            conn->connector_type,
            crtc_id,
            conn->connector_type_id,
            std::move(connector_name),
            conn->connector_id,
            conn->connection == DRM_MODE_CONNECTED,
          });
        });

        return monitors;
      }

      file_t handleFD(std::uint32_t handle) {
        file_t fb_fd;

        // For an imported GEM, DRM core deliberately returns
        // obj->import_attach->dmabuf here rather than exporting a new
        // shmem-backed object. Keep this PRIME step as the capture boundary:
        // the encoder GPU receives the original producer's DMA-BUF and its
        // modifier, preserving zero-copy NVIDIA scanout when EGL can import
        // that modifier. CLOEXEC prevents the per-frame fd from escaping into
        // child processes; it does not alter the underlying DMA-BUF object.
        auto status = drmPrimeHandleToFD(fd.el, handle, DRM_CLOEXEC /* flags */, &fb_fd.el);
        if (status) {
          return {};
        }

        return fb_fd;
      }

      std::vector<std::pair<prop_t, std::uint64_t>> props(std::uint32_t id, std::uint32_t type) {
        obj_prop_t obj_prop = drmModeObjectGetProperties(fd.el, id, type);
        if (!obj_prop) {
          return {};
        }

        std::vector<std::pair<prop_t, std::uint64_t>> props;
        props.reserve(obj_prop->count_props);

        for (auto x = 0; x < obj_prop->count_props; ++x) {
          props.emplace_back(drmModeGetProperty(fd.el, obj_prop->props[x]), obj_prop->prop_values[x]);
        }

        return props;
      }

      std::vector<std::pair<prop_t, std::uint64_t>> plane_props(std::uint32_t id) {
        return props(id, DRM_MODE_OBJECT_PLANE);
      }

      std::vector<std::pair<prop_t, std::uint64_t>> crtc_props(std::uint32_t id) {
        return props(id, DRM_MODE_OBJECT_CRTC);
      }

      std::vector<std::pair<prop_t, std::uint64_t>> connector_props(std::uint32_t id) {
        return props(id, DRM_MODE_OBJECT_CONNECTOR);
      }

      plane_t operator[](std::uint32_t index) {
        return drmModeGetPlane(fd.el, plane_res->planes[index]);
      }

      std::uint32_t count() {
        return plane_res->count_planes;
      }

      plane_it_t begin() const {
        return plane_it_t {fd.el, plane_res->planes, plane_res->planes + plane_res->count_planes};
      }

      plane_it_t end() const {
        return plane_it_t {fd.el, plane_res->planes + plane_res->count_planes};
      }

      file_t fd;
      file_t render_fd;
      plane_res_t plane_res;
      std::string driver_name;
    };

    std::map<std::uint32_t, monitor_t> map_crtc_to_monitor(const std::vector<connector_t> &connectors) {
      std::map<std::uint32_t, monitor_t> result;

      for (auto &connector : connectors) {
        // Disconnected connectors have no CRTC and would all collide at key 0.
        // Only an active CRTC can have a framebuffer that kmsgrab can capture.
        if (!connector.crtc_id) {
          continue;
        }
        result.emplace(connector.crtc_id, monitor_t {
                                            connector.type,
                                            connector.index,
                                            connector.name,
                                          });
      }

      return result;
    }

    struct kms_img_t: public img_t {
      ~kms_img_t() override {
        delete[] data;
        data = nullptr;
      }
    };

    void print(plane_t::pointer plane, fb_t::pointer fb, crtc_t::pointer crtc) {
      if (crtc) {
        BOOST_LOG(debug) << "crtc("sv << crtc->x << ", "sv << crtc->y << ')';
        BOOST_LOG(debug) << "crtc("sv << crtc->width << ", "sv << crtc->height << ')';
        BOOST_LOG(debug) << "plane->possible_crtcs == "sv << plane->possible_crtcs;
      }

      BOOST_LOG(debug)
        << "x("sv << plane->x
        << ") y("sv << plane->y
        << ") crtc_x("sv << plane->crtc_x
        << ") crtc_y("sv << plane->crtc_y
        << ") crtc_id("sv << plane->crtc_id
        << ')';

      BOOST_LOG(debug)
        << "Resolution: "sv << fb->width << 'x' << fb->height
        << ": Pitch: "sv << fb->pitches[0]
        << ": Offset: "sv << fb->offsets[0];

      std::stringstream ss;

      ss << "Format ["sv;
      std::for_each_n(plane->formats, plane->count_formats - 1, [&ss](auto format) {
        ss << util::view(format) << ", "sv;
      });

      ss << util::view(plane->formats[plane->count_formats - 1]) << ']';

      BOOST_LOG(debug) << ss.str();
    }

    class display_t: public platf::display_t {
    public:
      struct exported_frame_t {
        vibeshine_drm_frame descriptor {};
        std::array<file_t, VIBESHINE_DRM_FRAME_MAX_PLANES> dma_buf_fds;
        std::array<file_t, VIBESHINE_DRM_FRAME_MAX_PLANES> sync_files;
        std::chrono::steady_clock::time_point timestamp;
      };

      enum class frame_export_e {
        ready,
        empty,
        unsupported,
      };

      display_t(mem_type_e mem_type):
          platf::display_t(),
          mem_type {mem_type} {
      }

      int init(const std::string &display_name, const ::video::config_t &config) {
        delay = std::chrono::nanoseconds {1s} / config.framerate;

        // Empty historically selected monitor 0. Explicit decimal names remain
        // legacy aliases, while every other value resolves through the stable
        // connector names produced by kms_display_names().
        auto numeric_alias = display_name.empty() ? std::optional<std::uint32_t> {0} : selection::parse_numeric_alias(display_name);
        std::optional<selection::monitor_t> selected_monitor;
        if (!numeric_alias) {
          selected_monitor = selection::resolve_named_monitor(display_name, named_monitors);
          if (!selected_monitor) {
            BOOST_LOG(error) << "Couldn't resolve DRM connector ["sv << display_name << "]";
            return -1;
          }

          BOOST_LOG(debug) << "Resolved DRM connector ["sv << display_name << "] to monitor ["sv
                           << selected_monitor->monitor_index << "] on "sv << selected_monitor->card_path;
        }

        std::uint32_t monitor = 0;

        fs::path card_dir {"/dev/dri"sv};
        for (auto &entry : fs::directory_iterator {card_dir}) {
          auto file = entry.path().filename();

          auto filestring = file.generic_string();
          if (filestring.size() < 4 || std::string_view {filestring}.substr(0, 4) != "card"sv) {
            continue;
          }
          if (selected_monitor && selected_monitor->card_path != filestring) {
            continue;
          }

          kms::card_t card;
          if (card.init(entry.path().c_str())) {
            continue;
          }

          // Skip cards whose scanout buffers are not known to support the CUDA
          // import path unless NVENC was explicitly selected by the user.
          if (mem_type == mem_type_e::cuda && !card.supports_cuda_import()) {
            BOOST_LOG(debug) << file << " does not support CUDA framebuffer import"sv;
            if (config::video.encoder != "nvenc" && config::video.encoder != "nvenc_experimental") {
              continue;
            }
          }

          // Skip Nvidia cards if we're looking for VAAPI devices
          // This is important for hybrid GPU laptops where the display
          // may be connected through NVIDIA but rendering happens on Intel
          if (mem_type == mem_type_e::vaapi && card.is_nvidia()) {
            BOOST_LOG(debug) << file << " is an NVIDIA card, skipping for VAAPI"sv;
            continue;
          }

          auto end = std::end(card);
          for (auto plane = std::begin(card); plane != end; ++plane) {
            // Skip unused planes
            if (!plane->fb_id) {
              continue;
            }

            if (card.is_cursor(plane->plane_id)) {
              continue;
            }

            if (selected_monitor) {
              if (plane->crtc_id != selected_monitor->crtc_id) {
                continue;
              }
            } else {
              if (monitor != *numeric_alias) {
                ++monitor;
                continue;
              }
            }

            auto fb = card.fb(plane.get());
            if (!fb) {
              BOOST_LOG(error) << "Couldn't get drm fb for plane ["sv << plane->fb_id << "]: "sv << strerror(errno);
              return -1;
            }

            if (!fb->handles[0]) {
              BOOST_LOG(error) << "Couldn't get handle for DRM Framebuffer ["sv << plane->fb_id << "]: Probably not permitted"sv;
              return -1;
            }

            for (int i = 0; i < 4; ++i) {
              if (!fb->handles[i]) {
                break;
              }

              auto fb_fd = card.handleFD(fb->handles[i]);
              if (fb_fd.el < 0) {
                BOOST_LOG(error) << "Couldn't get primary file descriptor for Framebuffer ["sv << fb->fb_id << "]: "sv << strerror(errno);
                continue;
              }
            }

            auto crtc = card.crtc(plane->crtc_id);
            if (!crtc) {
              BOOST_LOG(error) << "Couldn't get CRTC info: "sv << strerror(errno);
              continue;
            }

            BOOST_LOG(info) << "Found monitor for DRM screencasting"sv;

            // We need to find the correct /dev/dri/card{nr} to correlate the crtc_id with the monitor descriptor
            auto pos = std::find_if(std::begin(card_descriptors), std::end(card_descriptors), [&](card_descriptor_t &cd) {
              return cd.path == filestring;
            });

            if (pos == std::end(card_descriptors)) {
              // This code path shouldn't happen, but it's there just in case.
              // card_descriptors is part of the guesswork after all.
              BOOST_LOG(error) << "Couldn't find ["sv << entry.path() << "]: This shouldn't have happened :/"sv;
              return -1;
            }

            // TODO: surf_sd = fb->to_sd();

            kms::print(plane.get(), fb.get(), crtc.get());

            img_width = fb->width;
            img_height = fb->height;
            img_offset_x = crtc->x;
            img_offset_y = crtc->y;

            this->env_width = ::platf::kms::env_width;
            this->env_height = ::platf::kms::env_height;

            this->env_logical_width = ::platf::kms::env_logical_width;
            this->env_logical_height = ::platf::kms::env_logical_height;

            auto monitor = pos->crtc_to_monitor.find(plane->crtc_id);
            if (monitor != std::end(pos->crtc_to_monitor)) {
              auto &viewport = monitor->second.viewport;

              width = viewport.width;
              height = viewport.height;

              logical_width = viewport.logical_width;
              logical_height = viewport.logical_height;

              switch (card.get_panel_orientation(plane->plane_id)) {
                case DRM_MODE_ROTATE_270:
                  BOOST_LOG(debug) << "Detected panel orientation at 90, swapping width and height.";
                  width = viewport.height;
                  height = viewport.width;
                  break;
                case DRM_MODE_ROTATE_90:
                case DRM_MODE_ROTATE_180:
                  BOOST_LOG(warning) << "Panel orientation is unsupported, screen capture may not work correctly.";
                  break;
              }

              offset_x = viewport.offset_x;
              offset_y = viewport.offset_y;
            }

            // This code path shouldn't happen, but it's there just in case.
            // crtc_to_monitor is part of the guesswork after all.
            else {
              BOOST_LOG(warning) << "Couldn't find crtc_id, this shouldn't have happened :\\"sv;
              width = crtc->width;
              height = crtc->height;
              offset_x = crtc->x;
              offset_y = crtc->y;
            }

            plane_id = plane->plane_id;
            crtc_id = plane->crtc_id;
            crtc_index = card.get_crtc_index_by_id(plane->crtc_id);

            // Find the connector for this CRTC
            for (auto &connector : card.monitors()) {
              if (connector.crtc_id == crtc_id) {
                BOOST_LOG(info) << "Found connector ID ["sv << connector.connector_id << ']';

                connector_id = connector.connector_id;

                auto connector_props = card.connector_props(*connector_id);
                hdr_metadata_blob_id = card.prop_value_by_name(connector_props, "HDR_OUTPUT_METADATA"sv);
              }
            }

            direct_import_required = card.requires_direct_import();
            this->card = std::move(card);
            goto break_loop;
          }
        }

        BOOST_LOG(error) << "Couldn't find monitor ["sv << display_name << ']';
        return -1;

      // Neatly break from nested for loop
      break_loop:

        // Look for the cursor plane for this CRTC
        cursor_plane_id = -1;
        auto end = std::end(card);
        for (auto plane = std::begin(card); plane != end; ++plane) {
          if (!card.is_cursor(plane->plane_id)) {
            continue;
          }

          // NB: We do not skip unused planes here because cursor planes
          // will look unused if the cursor is currently hidden.

          if (!(plane->possible_crtcs & (1 << crtc_index))) {
            // Skip cursor planes for other CRTCs
            continue;
          } else if (plane->possible_crtcs != (1 << crtc_index)) {
            // We assume a 1:1 mapping between cursor planes and CRTCs, which seems to
            // match the behavior of drivers in the real world. If it's violated, we'll
            // proceed anyway but print a warning in the log.
            BOOST_LOG(warning) << "Cursor plane spans multiple CRTCs!"sv;
          }

          BOOST_LOG(info) << "Found cursor plane ["sv << plane->plane_id << ']';
          cursor_plane_id = plane->plane_id;
          break;
        }

        if (cursor_plane_id < 0) {
          BOOST_LOG(warning) << "No KMS cursor plane found. Cursor may not be displayed while streaming!"sv;
        }

        initialize_presentation_events();

        return 0;
      }

      capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
        if (presentation_mode.event_capture_enabled()) {
          if (auto result = capture_presentation_events(push_captured_image_cb, pull_free_image_cb, cursor)) {
            return *result;
          }
          BOOST_LOG(error) << "Vibeshine DRM presentation events became unavailable; refusing fixed-rate KMS fallback."sv;
          presentation_mode.deactivate();
          return capture_e::error;
        }

        if (!presentation_mode.fixed_rate_allowed()) {
          return capture_e::error;
        }

        return capture_fixed_rate(push_captured_image_cb, pull_free_image_cb, cursor);
      }

      void request_refresh() override {
        if (presentation_mode.event_capture_enabled()) {
          presentation_latch.request_capture();
          if (!presentation_pending && presentation_latch.capture_ready()) {
            presentation_timestamp.reset();
            presentation_pending = true;
          }
        }
      }

      bool is_hdr() {
        if (!hdr_metadata_blob_id || *hdr_metadata_blob_id == 0) {
          return false;
        }

        prop_blob_t hdr_metadata_blob = drmModeGetPropertyBlob(card.fd.el, *hdr_metadata_blob_id);
        if (hdr_metadata_blob == nullptr) {
          BOOST_LOG(error) << "Unable to get HDR metadata blob: "sv << strerror(errno);
          return false;
        }

        if (hdr_metadata_blob->length < sizeof(uint32_t) + sizeof(hdr_metadata_infoframe)) {
          BOOST_LOG(error) << "HDR metadata blob is too small: "sv << hdr_metadata_blob->length;
          return false;
        }

        auto raw_metadata = (hdr_output_metadata *) hdr_metadata_blob->data;
        if (raw_metadata->metadata_type != 0) {  // HDMI_STATIC_METADATA_TYPE1
          BOOST_LOG(error) << "Unknown HDMI_STATIC_METADATA_TYPE value: "sv << raw_metadata->metadata_type;
          return false;
        }

        if (raw_metadata->hdmi_metadata_type1.metadata_type != 0) {  // Static Metadata Type 1
          BOOST_LOG(error) << "Unknown secondary metadata type value: "sv << raw_metadata->hdmi_metadata_type1.metadata_type;
          return false;
        }

        // HDR10 capture requires SMPTE ST 2084. Do not mislabel traditional
        // gamma HDR or HLG scanout as PQ on the encoded stream.
        switch (raw_metadata->hdmi_metadata_type1.eotf) {
          case 0:  // HDMI_EOTF_TRADITIONAL_GAMMA_SDR
            return false;
          case 1:  // HDMI_EOTF_TRADITIONAL_GAMMA_HDR
            BOOST_LOG(warning) << "Unsupported HDR EOTF: Traditional Gamma"sv;
            return false;
          case 2:  // HDMI_EOTF_SMPTE_ST2084
            return linux_hdr::is_hdr10_eotf(raw_metadata->hdmi_metadata_type1.eotf);
          case 3:  // HDMI_EOTF_BT_2100_HLG
            BOOST_LOG(warning) << "Unsupported HDR EOTF: HLG"sv;
            return false;
          default:
            BOOST_LOG(warning) << "Unsupported HDR EOTF: "sv << raw_metadata->hdmi_metadata_type1.eotf;
            return false;
        }
      }

      bool get_hdr_metadata(SS_HDR_METADATA &metadata) {
        // This performs all the metadata validation
        if (!is_hdr()) {
          return false;
        }

        prop_blob_t hdr_metadata_blob = drmModeGetPropertyBlob(card.fd.el, *hdr_metadata_blob_id);
        if (hdr_metadata_blob == nullptr) {
          BOOST_LOG(error) << "Unable to get HDR metadata blob: "sv << strerror(errno);
          return false;
        }

        auto raw_metadata = (hdr_output_metadata *) hdr_metadata_blob->data;

        for (int i = 0; i < 3; i++) {
          metadata.displayPrimaries[i].x = raw_metadata->hdmi_metadata_type1.display_primaries[i].x;
          metadata.displayPrimaries[i].y = raw_metadata->hdmi_metadata_type1.display_primaries[i].y;
        }

        metadata.whitePoint.x = raw_metadata->hdmi_metadata_type1.white_point.x;
        metadata.whitePoint.y = raw_metadata->hdmi_metadata_type1.white_point.y;
        metadata.maxDisplayLuminance = raw_metadata->hdmi_metadata_type1.max_display_mastering_luminance;
        metadata.minDisplayLuminance = raw_metadata->hdmi_metadata_type1.min_display_mastering_luminance;
        metadata.maxContentLightLevel = raw_metadata->hdmi_metadata_type1.max_cll;
        metadata.maxFrameAverageLightLevel = raw_metadata->hdmi_metadata_type1.max_fall;

        return true;
      }

      void update_cursor() {
        if (cursor_plane_id < 0) {
          return;
        }

        plane_t plane = drmModeGetPlane(card.fd.el, cursor_plane_id);

        std::optional<std::int32_t> prop_crtc_x;
        std::optional<std::int32_t> prop_crtc_y;
        std::optional<std::uint32_t> prop_crtc_w;
        std::optional<std::uint32_t> prop_crtc_h;

        std::optional<std::uint64_t> prop_src_x;
        std::optional<std::uint64_t> prop_src_y;
        std::optional<std::uint64_t> prop_src_w;
        std::optional<std::uint64_t> prop_src_h;

        auto props = card.plane_props(cursor_plane_id);
        for (auto &[prop, val] : props) {
          if (prop->name == "CRTC_X"sv) {
            prop_crtc_x = val;
          } else if (prop->name == "CRTC_Y"sv) {
            prop_crtc_y = val;
          } else if (prop->name == "CRTC_W"sv) {
            prop_crtc_w = val;
          } else if (prop->name == "CRTC_H"sv) {
            prop_crtc_h = val;
          } else if (prop->name == "SRC_X"sv) {
            prop_src_x = val;
          } else if (prop->name == "SRC_Y"sv) {
            prop_src_y = val;
          } else if (prop->name == "SRC_W"sv) {
            prop_src_w = val;
          } else if (prop->name == "SRC_H"sv) {
            prop_src_h = val;
          }
        }

        if (!prop_crtc_w || !prop_crtc_h || !prop_crtc_x || !prop_crtc_y) {
          BOOST_LOG(error) << "Cursor plane is missing required plane CRTC properties!"sv;
          BOOST_LOG(error) << "Atomic mode-setting must be enabled to capture the cursor!"sv;
          cursor_plane_id = -1;
          captured_cursor.visible = false;
          return;
        }
        if (!prop_src_x || !prop_src_y || !prop_src_w || !prop_src_h) {
          BOOST_LOG(error) << "Cursor plane is missing required plane SRC properties!"sv;
          BOOST_LOG(error) << "Atomic mode-setting must be enabled to capture the cursor!"sv;
          cursor_plane_id = -1;
          captured_cursor.visible = false;
          return;
        }

        // Update the cursor position and size unconditionally
        captured_cursor.x = *prop_crtc_x;
        captured_cursor.y = *prop_crtc_y;
        captured_cursor.dst_w = *prop_crtc_w;
        captured_cursor.dst_h = *prop_crtc_h;

        // We're technically cheating a bit here by assuming that we can detect
        // changes to the cursor plane via property adjustments. If this isn't
        // true, we'll really have to mmap() the dmabuf and draw that every time.
        bool cursor_dirty = false;

        if (!plane->fb_id) {
          captured_cursor.visible = false;
          captured_cursor.fb_id = 0;
        } else if (plane->fb_id != captured_cursor.fb_id) {
          BOOST_LOG(debug) << "Refreshing cursor image after FB changed"sv;
          cursor_dirty = true;
        } else if (*prop_src_x != captured_cursor.prop_src_x || *prop_src_y != captured_cursor.prop_src_y || *prop_src_w != captured_cursor.prop_src_w || *prop_src_h != captured_cursor.prop_src_h) {
          BOOST_LOG(debug) << "Refreshing cursor image after source dimensions changed"sv;
          cursor_dirty = true;
        }

        // If the cursor is dirty, map it so we can download the new image
        if (cursor_dirty) {
          auto fb = card.fb(plane.get());
          if (!fb || !fb->handles[0]) {
            // This means the cursor is not currently visible
            captured_cursor.visible = false;
            return;
          }

          // All known cursor planes in the wild are ARGB8888
          if (fb->pixel_format != DRM_FORMAT_ARGB8888) {
            BOOST_LOG(error) << "Unsupported non-ARGB8888 cursor format: "sv << fb->pixel_format;
            captured_cursor.visible = false;
            cursor_plane_id = -1;
            return;
          }

          // All known cursor planes in the wild require linear buffers
          if (fb->modifier != DRM_FORMAT_MOD_LINEAR && fb->modifier != DRM_FORMAT_MOD_INVALID) {
            BOOST_LOG(error) << "Unsupported non-linear cursor modifier: "sv << fb->modifier;
            captured_cursor.visible = false;
            cursor_plane_id = -1;
            return;
          }

          // The SRC_* properties are in Q16.16 fixed point, so convert to integers
          auto src_x = *prop_src_x >> 16;
          auto src_y = *prop_src_y >> 16;
          auto src_w = *prop_src_w >> 16;
          auto src_h = *prop_src_h >> 16;

          // Check for a legal source rectangle
          if (src_x + src_w > fb->width || src_y + src_h > fb->height) {
            BOOST_LOG(error) << "Illegal source size: ["sv << src_x + src_w << ',' << src_y + src_h << "] > ["sv << fb->width << ',' << fb->height << ']';
            captured_cursor.visible = false;
            return;
          }

          file_t plane_fd = card.handleFD(fb->handles[0]);
          if (plane_fd.el < 0) {
            captured_cursor.visible = false;
            return;
          }

          // We will map the entire region, but only copy what the source rectangle specifies
          size_t mapped_size = ((size_t) fb->pitches[0]) * fb->height;
          void *mapped_data = mmap(nullptr, mapped_size, PROT_READ, MAP_SHARED, plane_fd.el, fb->offsets[0]);

          // If we got ENOSYS back, let's try to map it as a dumb buffer instead (required for Nvidia GPUs)
          if (mapped_data == MAP_FAILED && errno == ENOSYS) {
            drm_mode_map_dumb map = {};
            map.handle = fb->handles[0];
            if (drmIoctl(card.fd.el, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
              BOOST_LOG(error) << "Failed to map cursor FB as dumb buffer: "sv << strerror(errno);
              captured_cursor.visible = false;
              return;
            }

            mapped_data = mmap(nullptr, mapped_size, PROT_READ, MAP_SHARED, card.fd.el, map.offset);
          }

          if (mapped_data == MAP_FAILED) {
            BOOST_LOG(error) << "Failed to mmap cursor FB: "sv << strerror(errno);
            captured_cursor.visible = false;
            return;
          }

          captured_cursor.pixels.resize(src_w * src_h * 4);

          // Prepare to read the dmabuf from the CPU
          struct dma_buf_sync sync;
          sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
          drmIoctl(plane_fd.el, DMA_BUF_IOCTL_SYNC, &sync);

          // If the image is tightly packed, copy it in one shot
          if (fb->pitches[0] == src_w * 4 && src_x == 0) {
            memcpy(captured_cursor.pixels.data(), &((std::uint8_t *) mapped_data)[src_y * fb->pitches[0]], src_h * fb->pitches[0]);
          } else {
            // Copy row by row to deal with mismatched pitch or an X offset
            auto pixel_dst = captured_cursor.pixels.data();
            for (int y = 0; y < src_h; y++) {
              memcpy(&pixel_dst[y * (src_w * 4)], &((std::uint8_t *) mapped_data)[(y + src_y) * fb->pitches[0] + (src_x * 4)], src_w * 4);
            }
          }

          // End the CPU read and unmap the dmabuf
          sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
          drmIoctl(plane_fd.el, DMA_BUF_IOCTL_SYNC, &sync);

          munmap(mapped_data, mapped_size);

          captured_cursor.visible = true;
          captured_cursor.src_w = src_w;
          captured_cursor.src_h = src_h;
          captured_cursor.prop_src_x = *prop_src_x;
          captured_cursor.prop_src_y = *prop_src_y;
          captured_cursor.prop_src_w = *prop_src_w;
          captured_cursor.prop_src_h = *prop_src_h;
          captured_cursor.fb_id = plane->fb_id;
          ++captured_cursor.serial;
        }
      }

      inline capture_e refresh(file_t *file, egl::surface_descriptor_t *sd, std::optional<std::chrono::steady_clock::time_point> &frame_timestamp) {
        // Check for a change in HDR metadata
        if (connector_id) {
          auto connector_props = card.connector_props(*connector_id);
          if (hdr_metadata_blob_id != card.prop_value_by_name(connector_props, "HDR_OUTPUT_METADATA"sv)) {
            BOOST_LOG(info) << "Reinitializing capture after HDR metadata change"sv;
            return capture_e::reinit;
          }
        }

        if (pending_exported_frame) {
          auto exported = std::move(*pending_exported_frame);
          pending_exported_frame.reset();
          const auto &frame = exported.descriptor;

          std::fill_n(sd->fds, VIBESHINE_DRM_FRAME_MAX_PLANES, -1);
          for (std::uint32_t plane = 0; plane < frame.plane_count; ++plane) {
            file[plane] = std::move(exported.dma_buf_fds[plane]);
            sd->fds[plane] = file[plane].el;
            sd->offsets[plane] = frame.offsets[plane];
            sd->pitches[plane] = frame.pitches[plane];
          }

          sd->width = frame.width;
          sd->height = frame.height;
          sd->modifier = frame.modifier;
          sd->fourcc = frame.fourcc;
          sd->direct_import_required = true;
          frame_timestamp = exported.timestamp;

          if (frame.width != img_width || frame.height != img_height) {
            return capture_e::reinit;
          }

          update_cursor();
          return capture_e::ok;
        }

        plane_t plane = drmModeGetPlane(card.fd.el, plane_id);
        frame_timestamp = std::chrono::steady_clock::now();

        // A disconnected output temporarily has no plane framebuffer. End this
        // capture generation instead of retrying and logging at the frame rate.
        if (!plane || plane->fb_id == 0) {
          return capture_e::reinit;
        }

        auto fb = card.fb(plane.get());
        if (!fb) {
          // This can happen if the display is being reconfigured while streaming
          BOOST_LOG(warning) << "Couldn't get drm fb for plane ["sv << plane->fb_id << "]: "sv << strerror(errno);
          return capture_e::timeout;
        }

        if (!fb->handles[0]) {
          BOOST_LOG(error) << "Couldn't get handle for DRM Framebuffer ["sv << plane->fb_id << "]: Probably not permitted"sv;
          return capture_e::error;
        }

        for (int y = 0; y < 4; ++y) {
          if (!fb->handles[y]) {
            // setting sd->fds[y] to a negative value indicates that sd->offsets[y] and sd->pitches[y]
            // are uninitialized and contain invalid values.
            sd->fds[y] = -1;
            // It's not clear whether there could still be valid handles left.
            // So, continue anyway.
            // TODO: Is this redundant?
            continue;
          }

          file[y] = card.handleFD(fb->handles[y]);
          if (file[y].el < 0) {
            BOOST_LOG(error) << "Couldn't get primary file descriptor for Framebuffer ["sv << fb->fb_id << "]: "sv << strerror(errno);
            return capture_e::error;
          }

          sd->fds[y] = file[y].el;
          sd->offsets[y] = fb->offsets[y];
          sd->pitches[y] = fb->pitches[y];
        }

        sd->width = fb->width;
        sd->height = fb->height;
        sd->modifier = fb->modifier;
        sd->fourcc = fb->pixel_format;
        sd->direct_import_required = direct_import_required;

        if (
          fb->width != img_width ||
          fb->height != img_height
        ) {
          return capture_e::reinit;
        }

        update_cursor();

        return capture_e::ok;
      }

      void update_crtc_gamma_lut(egl::img_descriptor_t &img) {
        const auto crtc_properties = card.crtc_props(crtc_id);
        const auto blob_id = card.prop_value_by_name(crtc_properties, "GAMMA_LUT"sv).value_or(0);
        if (blob_id != crtc_gamma_lut_blob_id) {
          crtc_gamma_lut_blob_id = blob_id;
          crtc_gamma_lut.reset();

          if (blob_id != 0) {
            prop_blob_t blob = drmModeGetPropertyBlob(card.fd.el, blob_id);
            if (!blob || blob->length < 2 * sizeof(drm_color_lut) || blob->length % sizeof(drm_color_lut) != 0) {
              BOOST_LOG(warning) << "Ignoring invalid CRTC GAMMA_LUT blob ["sv << blob_id << ']';
            } else {
              const auto count = blob->length / sizeof(drm_color_lut);
              auto lut = std::make_shared<egl::img_descriptor_t::gamma_lut_t>();
              lut->reserve(count);
              const auto *entries = static_cast<const drm_color_lut *>(blob->data);
              for (std::size_t index = 0; index < count; ++index) {
                lut->push_back({entries[index].red, entries[index].green, entries[index].blue});
              }
              crtc_gamma_lut = std::move(lut);
              BOOST_LOG(info) << "Applying "sv << count << "-entry CRTC GAMMA_LUT during KMS capture."sv;
            }
          }
        }

        img.crtc_gamma_lut = crtc_gamma_lut;
        img.crtc_gamma_lut_serial = crtc_gamma_lut_blob_id;
      }

      enum class presentation_wait_e {
        changed,
        timeout,
        unsupported,
      };

      frame_export_e dequeue_presentation_frame() {
        vibeshine_drm_frame request {};
        request.abi_version = VIBESHINE_DRM_FRAME_ABI_VERSION;
        request.crtc_id = static_cast<std::uint32_t>(crtc_id);

        cap_sys_admin admin;
        if (::ioctl(card.fd.el, DRM_IOCTL_VIBESHINE_GET_FRAME, &request) < 0) {
          BOOST_LOG(error) << "Failed to export Vibeshine DRM presentation frame: "sv << strerror(errno);
          return frame_export_e::unsupported;
        }

        exported_frame_t exported;
        exported.descriptor = request;
        for (std::size_t plane = 0; plane < exported.dma_buf_fds.size(); ++plane) {
          exported.dma_buf_fds[plane].el = request.dma_buf_fds[plane];
          exported.sync_files[plane].el = request.sync_file_fds[plane];
        }

        const bool common_fields_valid =
          request.abi_version == VIBESHINE_DRM_FRAME_ABI_VERSION &&
          request.crtc_id == static_cast<std::uint32_t>(crtc_id) &&
          request.reserved_u32 == 0 &&
          std::ranges::all_of(request.reserved, [](std::uint64_t value) { return value == 0; });
        if (!common_fields_valid) {
          BOOST_LOG(error) << "Vibeshine DRM returned an invalid frame ABI response."sv;
          return frame_export_e::unsupported;
        }

        if (request.flags == VIBESHINE_DRM_FRAME_EMPTY) {
          const bool empty_descriptor = request.width == 0 && request.height == 0 &&
                                        request.fourcc == 0 && request.modifier == 0 &&
                                        request.plane_count == 0 &&
                                        std::ranges::all_of(request.dma_buf_fds, [](std::int32_t fd) { return fd == -1; }) &&
                                        std::ranges::all_of(request.sync_file_fds, [](std::int32_t fd) { return fd == -1; }) &&
                                        std::ranges::all_of(request.pitches, [](std::uint32_t value) { return value == 0; }) &&
                                        std::ranges::all_of(request.offsets, [](std::uint32_t value) { return value == 0; });
          if (!empty_descriptor) {
            BOOST_LOG(error) << "Vibeshine DRM returned a malformed empty frame."sv;
            return frame_export_e::unsupported;
          }
          pending_exported_frame.reset();
          return frame_export_e::empty;
        }

        if (request.flags != VIBESHINE_DRM_FRAME_READY || request.sequence == 0 ||
            request.width == 0 || request.height == 0 || request.fourcc == 0 ||
            request.plane_count == 0 || request.plane_count > VIBESHINE_DRM_FRAME_MAX_PLANES) {
          BOOST_LOG(error) << "Vibeshine DRM returned a malformed presentation frame."sv;
          return frame_export_e::unsupported;
        }

        for (std::uint32_t plane = 0; plane < VIBESHINE_DRM_FRAME_MAX_PLANES; ++plane) {
          const bool active = plane < request.plane_count;
          if ((active && (request.dma_buf_fds[plane] < 0 || request.sync_file_fds[plane] < -1)) ||
              (!active && (request.dma_buf_fds[plane] != -1 || request.sync_file_fds[plane] != -1 ||
                           request.pitches[plane] != 0 || request.offsets[plane] != 0))) {
            BOOST_LOG(error) << "Vibeshine DRM returned an invalid DMA-BUF plane descriptor."sv;
            return frame_export_e::unsupported;
          }
        }

        auto timestamp = pacing::validate_timestamp(
          request.timestamp_ns,
          std::chrono::steady_clock::now(),
          0ns,
          last_presentation_timestamp
        );
        if (!timestamp) {
          BOOST_LOG(error) << "Vibeshine DRM returned an invalid frame presentation timestamp."sv;
          return frame_export_e::unsupported;
        }

        exported.timestamp = *timestamp;
        presentation_sequence = request.sequence;
        presentation_timestamp = *timestamp;
        last_presentation_timestamp = *timestamp;
        pending_exported_frame = std::move(exported);
        return frame_export_e::ready;
      }

      void initialize_presentation_events() {
        presentation_mode = pacing::presentation_mode_t {card.requires_presentation_events()};
        if (presentation_mode.fixed_rate_allowed()) {
          return;
        }

        presentation_sequence = 0;
        if (wait_for_presentation(0ms) == presentation_wait_e::unsupported) {
          BOOST_LOG(error) << "The loaded vibeshine_drm module does not provide a valid presentation ABI. Rebuild and reload the module; refusing fixed-rate KMS fallback."sv;
          return;
        }
        if (dequeue_presentation_frame() != frame_export_e::ready) {
          BOOST_LOG(error) << "The loaded vibeshine_drm module cannot export its completed primary framebuffer; refusing KMS polling fallback."sv;
          return;
        }

        presentation_mode.activate();
        presentation_latch.request_capture();
        presentation_pending = presentation_latch.capture_ready();
        BOOST_LOG(info) << "Using event-driven KMS capture for Vibeshine DRM CRTC ["sv << crtc_id << "]."sv;
      }

      presentation_wait_e wait_for_presentation(std::chrono::milliseconds timeout) {
        const auto requested_sequence = presentation_sequence;
        const auto wait_deadline = std::chrono::steady_clock::now() + timeout;
        unsigned int retry_count = 0;

        while (true) {
          auto remaining = timeout;
          if (timeout > 0ms) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= wait_deadline) {
              if (retry_count > 0) {
                return wait_for_presentation(0ms);
              }
              return presentation_wait_e::timeout;
            }
            remaining = std::chrono::ceil<std::chrono::milliseconds>(wait_deadline - now);
          }

          vibeshine_drm_wait_present request {};
          const auto requested_timeout_ms = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            remaining.count(),
            0,
            VIBESHINE_DRM_PRESENT_MAX_TIMEOUT_MS
          ));
          request.abi_version = VIBESHINE_DRM_PRESENT_ABI_VERSION;
          request.crtc_id = static_cast<std::uint32_t>(crtc_id);
          request.sequence = requested_sequence;
          request.timeout_ms = requested_timeout_ms;

          if (::ioctl(card.fd.el, DRM_IOCTL_VIBESHINE_WAIT_PRESENT, &request) < 0) {
            switch (pacing::classify_ioctl_error(errno, timeout > 0ms)) {
              case pacing::presentation_ioctl_error_e::retry:
                {
                  ++retry_count;
                  if (retry_count >= 4) {
                    std::this_thread::sleep_until(wait_deadline);
                    return wait_for_presentation(0ms);
                  }
                  const auto backoff = std::chrono::milliseconds {1U << (retry_count - 1)};
                  const auto retry_at = std::min(wait_deadline, std::chrono::steady_clock::now() + backoff);
                  std::this_thread::sleep_until(retry_at);
                  continue;
                }
              case pacing::presentation_ioctl_error_e::transient_timeout:
                std::this_thread::sleep_until(wait_deadline);
                return wait_for_presentation(0ms);
              case pacing::presentation_ioctl_error_e::unsupported:
                presentation_mode.deactivate();
                return presentation_wait_e::unsupported;
            }
          }

          if (request.abi_version != VIBESHINE_DRM_PRESENT_ABI_VERSION ||
              request.crtc_id != static_cast<std::uint32_t>(crtc_id) ||
              request.timeout_ms != requested_timeout_ms ||
              request.reserved[0] != 0 || request.reserved[1] != 0) {
            presentation_mode.deactivate();
            return presentation_wait_e::unsupported;
          }

          switch (pacing::classify_response(
            request.flags,
            VIBESHINE_DRM_PRESENT_CHANGED,
            VIBESHINE_DRM_PRESENT_TIMEOUT,
            VIBESHINE_DRM_PRESENT_PENDING,
            requested_sequence,
            request.sequence
          )) {
            case pacing::presentation_response_e::changed:
              {
                auto timestamp = pacing::validate_timestamp(
                  request.timestamp_ns,
                  std::chrono::steady_clock::now(),
                  0ns,
                  last_presentation_timestamp
                );
                if (!timestamp) {
                  presentation_mode.deactivate();
                  return presentation_wait_e::unsupported;
                }
                presentation_sequence = request.sequence;
                presentation_timestamp = *timestamp;
                last_presentation_timestamp = *timestamp;
                presentation_latch.observe_response(
                  pacing::presentation_response_e::changed,
                  (request.flags & VIBESHINE_DRM_PRESENT_PENDING) != 0,
                  std::chrono::steady_clock::now()
                );
                if (pacing::hold_pending_response_to_deadline(
                      pacing::presentation_response_e::changed,
                      (request.flags & VIBESHINE_DRM_PRESENT_PENDING) != 0,
                      timeout > 0ms
                    )) {
                  std::this_thread::sleep_until(wait_deadline);
                }
                return presentation_wait_e::changed;
              }
            case pacing::presentation_response_e::timeout:
              presentation_latch.observe_response(
                pacing::presentation_response_e::timeout,
                (request.flags & VIBESHINE_DRM_PRESENT_PENDING) != 0,
                std::chrono::steady_clock::now()
              );
              if (timeout > 0ms) {
                std::this_thread::sleep_until(wait_deadline);
              }
              return presentation_wait_e::timeout;
            case pacing::presentation_response_e::invalid:
              presentation_mode.deactivate();
              return presentation_wait_e::unsupported;
          }

          presentation_mode.deactivate();
          return presentation_wait_e::unsupported;
        }
      }

      std::optional<capture_e> capture_presentation_events(
        const push_captured_image_cb_t &push_captured_image_cb,
        const pull_free_image_cb_t &pull_free_image_cb,
        bool *cursor
      ) {
        auto next_capture = std::chrono::steady_clock::time_point::min();

        while (presentation_mode.event_capture_enabled()) {
          auto now = std::chrono::steady_clock::now();
          if (presentation_pending && now < next_capture) {
            std::this_thread::sleep_until(pacing::coalescing_wake_deadline(
              now,
              next_capture,
              PRESENT_WAIT_IDLE_TIMEOUT
            ));
            if (std::chrono::steady_clock::now() < next_capture &&
                !push_captured_image_cb({}, false)) {
              return platf::capture_e::ok;
            }
            continue;
          }

          if (pacing::capture_due(presentation_pending, now, next_capture)) {
            if (wait_for_presentation(0ms) == presentation_wait_e::unsupported) {
              return std::nullopt;
            }
            if (presentation_latch.state_pending()) {
              if (presentation_latch.pending_timed_out(std::chrono::steady_clock::now(), pacing::PRESENT_PENDING_HANG_TIMEOUT)) {
                return std::nullopt;
              }
              presentation_pending = false;
              if (!push_captured_image_cb({}, false)) {
                return platf::capture_e::ok;
              }
              continue;
            }

            if (!pending_exported_frame) {
              switch (dequeue_presentation_frame()) {
                case frame_export_e::ready:
                  break;
                case frame_export_e::empty:
                  return platf::capture_e::reinit;
                case frame_export_e::unsupported:
                  return std::nullopt;
              }
            }

            const auto captured_sequence = presentation_sequence;
            const auto captured_timestamp = presentation_timestamp;
            const auto captured_generation = presentation_latch.capture_generation();
            const auto capture_started = std::chrono::steady_clock::now();
            std::shared_ptr<platf::img_t> img_out;
            auto status = snapshot(pull_free_image_cb, img_out, 1000ms, *cursor);
            if (status == platf::capture_e::reinit ||
                status == platf::capture_e::error ||
                status == platf::capture_e::interrupted) {
              return status;
            }

            const auto post_capture_presentation = wait_for_presentation(0ms);
            if (post_capture_presentation == presentation_wait_e::unsupported) {
              img_out.reset();
              if (!push_captured_image_cb({}, false)) {
                return platf::capture_e::ok;
              }
              return std::nullopt;
            }

            next_capture = pacing::capture_deadline(capture_started, delay);
            if (presentation_latch.state_pending()) {
              if (presentation_latch.pending_timed_out(std::chrono::steady_clock::now(), pacing::PRESENT_PENDING_HANG_TIMEOUT)) {
                return std::nullopt;
              }
              presentation_pending = false;
              if (!push_captured_image_cb({}, false)) {
                return platf::capture_e::ok;
              }
              continue;
            }

            const bool newer_presentation = post_capture_presentation == presentation_wait_e::changed;
            if (!newer_presentation && presentation_sequence == captured_sequence &&
                status == platf::capture_e::ok && img_out && captured_timestamp) {
              img_out->frame_timestamp = captured_timestamp;
            }

            if (status == platf::capture_e::timeout && next_capture < std::chrono::steady_clock::now()) {
              next_capture = std::chrono::steady_clock::now() + delay;
            }

            switch (status) {
              case platf::capture_e::timeout:
                if (!push_captured_image_cb(std::move(img_out), false)) {
                  return platf::capture_e::ok;
                }
                break;
              case platf::capture_e::ok:
                presentation_latch.mark_delivered(captured_generation);
                presentation_pending = newer_presentation;
                if (!newer_presentation) {
                  presentation_timestamp.reset();
                }
                if (!push_captured_image_cb(std::move(img_out), true)) {
                  return platf::capture_e::ok;
                }
                break;
              case platf::capture_e::reinit:
              case platf::capture_e::error:
              case platf::capture_e::interrupted:
                return status;
              default:
                BOOST_LOG(error) << "Unrecognized capture status ["sv << static_cast<int>(status) << ']';
                return status;
            }
            continue;
          }

          switch (wait_for_presentation(PRESENT_WAIT_IDLE_TIMEOUT)) {
            case presentation_wait_e::changed:
            case presentation_wait_e::timeout:
              if (presentation_latch.state_pending() &&
                  presentation_latch.pending_timed_out(std::chrono::steady_clock::now(), pacing::PRESENT_PENDING_HANG_TIMEOUT)) {
                return std::nullopt;
              }
              if (presentation_latch.state_pending()) {
                presentation_pending = false;
                if (!push_captured_image_cb({}, false)) {
                  return platf::capture_e::ok;
                }
              } else if (presentation_latch.capture_ready()) {
                presentation_pending = true;
              } else if (!presentation_pending && !push_captured_image_cb({}, false)) {
                return platf::capture_e::ok;
              }
              break;
            case presentation_wait_e::unsupported:
              return std::nullopt;
          }
        }

        return std::nullopt;
      }

      capture_e capture_fixed_rate(
        const push_captured_image_cb_t &push_captured_image_cb,
        const pull_free_image_cb_t &pull_free_image_cb,
        bool *cursor
      ) {
        auto next_frame = std::chrono::steady_clock::now();

        sleep_overshoot_logger.reset();

        while (true) {
          auto now = std::chrono::steady_clock::now();

          if (next_frame > now) {
            std::this_thread::sleep_for(next_frame - now);
            sleep_overshoot_logger.first_point(next_frame);
            sleep_overshoot_logger.second_point_now_and_log();
          }

          next_frame += delay;
          if (next_frame < now) {
            next_frame = now + delay;
          }

          std::shared_ptr<platf::img_t> img_out;
          auto status = snapshot(pull_free_image_cb, img_out, 1000ms, *cursor);
          switch (status) {
            case platf::capture_e::reinit:
            case platf::capture_e::error:
            case platf::capture_e::interrupted:
              return status;
            case platf::capture_e::timeout:
              if (!push_captured_image_cb(std::move(img_out), false)) {
                return platf::capture_e::ok;
              }
              break;
            case platf::capture_e::ok:
              if (!push_captured_image_cb(std::move(img_out), true)) {
                return platf::capture_e::ok;
              }
              break;
            default:
              BOOST_LOG(error) << "Unrecognized capture status ["sv << static_cast<int>(status) << ']';
              return status;
          }
        }
      }

      virtual capture_e snapshot(
        const pull_free_image_cb_t &pull_free_image_cb,
        std::shared_ptr<platf::img_t> &img_out,
        std::chrono::milliseconds timeout,
        bool cursor
      ) = 0;

      mem_type_e mem_type;

      std::chrono::nanoseconds delay;

      int img_width;
      int img_height;
      int img_offset_x;
      int img_offset_y;

      int plane_id;
      int crtc_id;
      int crtc_index;

      std::optional<uint32_t> connector_id;
      std::optional<uint64_t> hdr_metadata_blob_id;
      bool direct_import_required {false};
      pacing::presentation_mode_t presentation_mode;
      bool presentation_pending {false};
      pacing::presentation_latch_t presentation_latch;
      std::uint64_t presentation_sequence {};
      std::optional<std::chrono::steady_clock::time_point> presentation_timestamp;
      std::optional<std::chrono::steady_clock::time_point> last_presentation_timestamp;
      std::optional<exported_frame_t> pending_exported_frame;
      std::uint64_t crtc_gamma_lut_blob_id {};
      std::shared_ptr<const egl::img_descriptor_t::gamma_lut_t> crtc_gamma_lut;

      int cursor_plane_id;
      cursor_t captured_cursor {};

      card_t card;
    };

    class display_ram_t: public display_t {
    public:
      display_ram_t(mem_type_e mem_type):
          display_t(mem_type) {
      }

      int init(const std::string &display_name, const ::video::config_t &config) {
        if (!gbm::create_device) {
          BOOST_LOG(warning) << "libgbm not initialized"sv;
          return -1;
        }

        if (display_t::init(display_name, config)) {
          return -1;
        }

        gbm.reset(gbm::create_device(card.fd.el));
        if (!gbm) {
          BOOST_LOG(error) << "Couldn't create GBM device: ["sv << util::hex(eglGetError()).to_string_view() << ']';
          return -1;
        }

        display = egl::make_display(gbm.get());
        if (!display) {
          return -1;
        }

        auto ctx_opt = egl::make_ctx(display.get());
        if (!ctx_opt) {
          return -1;
        }

        ctx = std::move(*ctx_opt);

        return 0;
      }

      std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override {
#ifdef SUNSHINE_BUILD_VAAPI
        if (mem_type == mem_type_e::vaapi) {
          return va::make_avcodec_encode_device(width, height, false);
        }
#endif

#ifdef SUNSHINE_BUILD_CUDA
        if (mem_type == mem_type_e::cuda) {
          return cuda::make_avcodec_encode_device(width, height, false);
        }
#endif

        return std::make_unique<avcodec_encode_device_t>();
      }

      void blend_cursor(img_t &img) {
        // TODO: Cursor scaling is not supported in this codepath.
        // We always draw the cursor at the source size.
        auto pixels = (int *) img.data;

        int32_t screen_height = img.height;
        int32_t screen_width = img.width;

        // This is the position in the target that we will start drawing the cursor
        auto cursor_x = std::max<int32_t>(0, captured_cursor.x - img_offset_x);
        auto cursor_y = std::max<int32_t>(0, captured_cursor.y - img_offset_y);

        // If the cursor is partially off screen, the coordinates may be negative
        // which means we will draw the top-right visible portion of the cursor only.
        auto cursor_delta_x = cursor_x - std::max<int32_t>(-captured_cursor.src_w, captured_cursor.x - img_offset_x);
        auto cursor_delta_y = cursor_y - std::max<int32_t>(-captured_cursor.src_h, captured_cursor.y - img_offset_y);

        auto delta_height = std::min<uint32_t>(captured_cursor.src_h, std::max<int32_t>(0, screen_height - cursor_y)) - cursor_delta_y;
        auto delta_width = std::min<uint32_t>(captured_cursor.src_w, std::max<int32_t>(0, screen_width - cursor_x)) - cursor_delta_x;
        for (auto y = 0; y < delta_height; ++y) {
          // Offset into the cursor image to skip drawing the parts of the cursor image that are off screen
          //
          // NB: We must access the elements via the data() function because cursor_end may point to the
          // the first element beyond the valid range of the vector. Using vector's [] operator in that
          // manner is undefined behavior (and triggers errors when using debug libc++), while doing the
          // same with an array is fine.
          auto cursor_begin = (uint32_t *) &captured_cursor.pixels.data()[((y + cursor_delta_y) * captured_cursor.src_w + cursor_delta_x) * 4];
          auto cursor_end = (uint32_t *) &captured_cursor.pixels.data()[((y + cursor_delta_y) * captured_cursor.src_w + delta_width + cursor_delta_x) * 4];

          auto pixels_begin = &pixels[(y + cursor_y) * (img.row_pitch / img.pixel_pitch) + cursor_x];

          std::for_each(cursor_begin, cursor_end, [&](uint32_t cursor_pixel) {
            auto colors_in = (uint8_t *) pixels_begin;

            auto alpha = (*(uint *) &cursor_pixel) >> 24u;
            if (alpha == 255) {
              *pixels_begin = cursor_pixel;
            } else {
              auto colors_out = (uint8_t *) &cursor_pixel;
              colors_in[0] = colors_out[0] + (colors_in[0] * (255 - alpha) + 255 / 2) / 255;
              colors_in[1] = colors_out[1] + (colors_in[1] * (255 - alpha) + 255 / 2) / 255;
              colors_in[2] = colors_out[2] + (colors_in[2] * (255 - alpha) + 255 / 2) / 255;
            }
            ++pixels_begin;
          });
        }
      }

      capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) override {
        const auto host_processing_timestamp = std::chrono::steady_clock::now();
        file_t fb_fd[4];

        egl::surface_descriptor_t sd;

        std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
        auto status = refresh(fb_fd, &sd, frame_timestamp);
        if (status != capture_e::ok) {
          return status;
        }

        auto rgb_opt = egl::import_source(display.get(), sd);

        if (!rgb_opt) {
          rgb_opt = egl::upload_source(display.get(), sd);
          if (!rgb_opt) {
            return capture_e::error;
          }
        }

        auto &rgb = *rgb_opt;

        gl::ctx.BindTexture(GL_TEXTURE_2D, rgb->tex[0]);

        // Don't remove these lines, see https://github.com/LizardByte/Sunshine/issues/453
        int h;
        int w;
        gl::ctx.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
        gl::ctx.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
        BOOST_LOG(debug) << "width and height: w "sv << w << " h "sv << h;

        if (!pull_free_image_cb(img_out)) {
          return platf::capture_e::interrupted;
        }

        gl::ctx.GetTextureSubImage(rgb->tex[0], 0, img_offset_x, img_offset_y, 0, width, height, 1, GL_BGRA, GL_UNSIGNED_BYTE, img_out->height * img_out->row_pitch, img_out->data);

        img_out->frame_timestamp = frame_timestamp;
        img_out->host_processing_timestamp = host_processing_timestamp;

        if (cursor && captured_cursor.visible) {
          blend_cursor(*img_out);
        }

        return capture_e::ok;
      }

      std::shared_ptr<img_t> alloc_img() override {
        auto img = std::make_shared<kms_img_t>();
        img->width = width;
        img->height = height;
        img->pixel_pitch = 4;
        img->row_pitch = img->pixel_pitch * width;
        img->data = new std::uint8_t[height * img->row_pitch];

        return img;
      }

      int dummy_img(platf::img_t *img) override {
        return 0;
      }

      gbm::gbm_t gbm;
      egl::display_t display;
      egl::ctx_t ctx;
    };

    class display_vram_t: public display_t {
    public:
      display_vram_t(mem_type_e mem_type):
          display_t(mem_type) {
      }

      std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override {
#ifdef SUNSHINE_BUILD_VAAPI
        if (mem_type == mem_type_e::vaapi) {
          return va::make_avcodec_encode_device(width, height, dup(card.render_fd.el), img_offset_x, img_offset_y, true);
        }
#endif

#ifdef SUNSHINE_BUILD_VULKAN
        if (mem_type == mem_type_e::vulkan) {
          return vk::make_avcodec_encode_device_vram(width, height, img_offset_x, img_offset_y);
        }
#endif

#ifdef SUNSHINE_BUILD_CUDA
        if (mem_type == mem_type_e::cuda) {
          return cuda::make_avcodec_gl_encode_device(width, height, img_offset_x, img_offset_y);
        }
#endif

        BOOST_LOG(error) << "Unsupported pixel format for egl::display_vram_t: "sv << platf::from_pix_fmt(pix_fmt);
        return nullptr;
      }

      std::unique_ptr<nvenc_encode_device_t> make_nvenc_encode_device(pix_fmt_e pix_fmt) override {
#ifdef SUNSHINE_BUILD_CUDA
        if (mem_type == mem_type_e::cuda) {
          return cuda::make_nvenc_gl_encode_device(width, height, img_offset_x, img_offset_y, pix_fmt);
        }
#endif
        return nullptr;
      }

      std::shared_ptr<img_t> alloc_img() override {
        auto img = std::make_shared<egl::img_descriptor_t>();

        img->width = width;
        img->height = height;
        img->serial = std::numeric_limits<decltype(img->serial)>::max();
        img->data = nullptr;
        img->pixel_pitch = 4;

        img->sequence = 0;
        std::fill_n(img->sd.fds, 4, -1);

        return img;
      }

      int dummy_img(platf::img_t *img) override {
        // Empty images are recognized as dummies by the zero sequence number
        return 0;
      }

      capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds /* timeout */, bool cursor) override {
        const auto host_processing_timestamp = std::chrono::steady_clock::now();
        file_t fb_fd[4];

        if (!pull_free_image_cb(img_out)) {
          return platf::capture_e::interrupted;
        }
        auto img = (egl::img_descriptor_t *) img_out.get();
        img->reset();

        auto status = refresh(fb_fd, &img->sd, img->frame_timestamp);
        if (status != capture_e::ok) {
          return status;
        }

        update_crtc_gamma_lut(*img);
        img->host_processing_timestamp = host_processing_timestamp;
        img->sequence = ++sequence;

        if (cursor && captured_cursor.visible) {
          // Copy new cursor pixel data if it's been updated
          if (img->serial != captured_cursor.serial) {
            img->buffer = captured_cursor.pixels;
            img->serial = captured_cursor.serial;
          }

          img->x = captured_cursor.x;
          img->y = captured_cursor.y;
          img->src_w = captured_cursor.src_w;
          img->src_h = captured_cursor.src_h;
          img->width = captured_cursor.dst_w;
          img->height = captured_cursor.dst_h;
          img->pixel_pitch = 4;
          img->row_pitch = img->pixel_pitch * img->width;
          img->data = img->buffer.data();
        } else {
          img->data = nullptr;
        }

        for (auto x = 0; x < 4; ++x) {
          fb_fd[x].release();
        }
        return capture_e::ok;
      }

      int init(const std::string &display_name, const ::video::config_t &config) {
        if (display_t::init(display_name, config)) {
          return -1;
        }

#ifdef SUNSHINE_BUILD_VAAPI
        if (mem_type == mem_type_e::vaapi && !va::validate(card.render_fd.el)) {
          BOOST_LOG(warning) << "Monitor "sv << display_name << " doesn't support hardware encoding. Reverting back to GPU -> RAM -> GPU"sv;
          return -1;
        }
#endif

#ifndef SUNSHINE_BUILD_CUDA
        if (mem_type == mem_type_e::cuda) {
          BOOST_LOG(warning) << "Attempting to use NVENC without CUDA support. Reverting back to GPU -> RAM -> GPU"sv;
          return -1;
        }
#endif

        return 0;
      }

      std::uint64_t sequence {};
    };

  }  // namespace kms

  std::shared_ptr<display_t> kms_display(mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
    if (hwdevice_type == mem_type_e::vaapi || hwdevice_type == mem_type_e::cuda || hwdevice_type == mem_type_e::vulkan) {
      auto disp = std::make_shared<kms::display_vram_t>(hwdevice_type);

      if (!disp->init(display_name, config)) {
        return disp;
      }

      // In the case of failure, attempt the old method for VAAPI
    }

    auto disp = std::make_shared<kms::display_ram_t>(hwdevice_type);

    if (disp->init(display_name, config)) {
      return nullptr;
    }

    return disp;
  }

  /**
   * On Wayland, it's not possible to determine the position of the monitor on the desktop with KMS.
   * Wayland does allow applications to query attached monitors on the desktop,
   * however, the naming scheme is not standardized across implementations.
   *
   * As a result, correlating the KMS output to the wayland outputs is guess work at best.
   * But, it's necessary for absolute mouse coordinates to work.
   *
   * This is an ugly hack :(
   */
  void correlate_to_wayland(std::vector<kms::card_descriptor_t> &cds) {
    auto monitors = wl::monitors();

    BOOST_LOG(info) << "-------- Start of KMS monitor list --------"sv;

    for (auto &monitor : monitors) {
      std::string_view name = monitor->name;
      std::vector<kms::monitor_t *> candidates;

      // Prefer the kernel connector name. This uses connector_type_id rather
      // than an enumeration counter, so it remains correct across DRM cards.
      for (auto &card_descriptor : cds) {
        for (auto &[_, monitor_descriptor] : card_descriptor.crtc_to_monitor) {
          if (monitor_descriptor.monitor_index && kms::selection::ascii_iequals(monitor_descriptor.connector_name, name)) {
            candidates.emplace_back(&monitor_descriptor);
          }
        }
      }

      // Some compositors use connector type aliases (for example HDMI-1).
      // Retain the old type/index fallback, now backed by the DRM-provided
      // connector_type_id rather than a process-global occurrence count.
      if (candidates.empty()) {
        const auto index_begin = name.find_last_of('-');
        const auto index = index_begin == std::string_view::npos ? 1 : std::max<int64_t>(1, util::from_view(name.substr(index_begin + 1)));
        const auto type = kms::from_view(name.substr(0, index_begin));

        for (auto &card_descriptor : cds) {
          for (auto &[_, monitor_descriptor] : card_descriptor.crtc_to_monitor) {
            if (monitor_descriptor.monitor_index && monitor_descriptor.index == index && monitor_descriptor.type == type) {
              candidates.emplace_back(&monitor_descriptor);
            }
          }
        }
      }

      // Matching dimensions can safely disambiguate identical connector names
      // exposed by different GPUs. If that is still ambiguous, do not attach
      // one output's desktop coordinates to another output.
      if (candidates.size() > 1) {
        std::erase_if(candidates, [&](const auto *candidate) {
          return candidate->viewport.width != monitor->viewport.width || candidate->viewport.height != monitor->viewport.height;
        });
      }

      if (candidates.size() != 1) {
        BOOST_LOG(warning) << "Couldn't uniquely correlate Wayland output ["sv << name << "] to a DRM connector"sv;
        continue;
      }

      auto &monitor_descriptor = *candidates.front();
      monitor_descriptor.viewport.offset_x = monitor->viewport.offset_x;
      monitor_descriptor.viewport.offset_y = monitor->viewport.offset_y;
      monitor_descriptor.viewport.logical_width = monitor->viewport.logical_width;
      monitor_descriptor.viewport.logical_height = monitor->viewport.logical_height;

      // A sanity check, it's guesswork after all.
      if (
        monitor_descriptor.viewport.width != monitor->viewport.width ||
        monitor_descriptor.viewport.height != monitor->viewport.height
      ) {
        BOOST_LOG(warning)
          << "Mismatch on expected Resolution compared to actual resolution: "sv
          << monitor_descriptor.viewport.width << 'x' << monitor_descriptor.viewport.height
          << " vs "sv
          << monitor->viewport.width << 'x' << monitor->viewport.height;
      }

      BOOST_LOG(info) << "Monitor " << *monitor_descriptor.monitor_index << " is "sv << name << ": "sv << monitor->description;
    }

    BOOST_LOG(info) << "--------- End of KMS monitor list ---------"sv;
  }

  // A list of names of displays accepted as display_name
  std::vector<std::string> kms_display_names(mem_type_e hwdevice_type) {
    std::uint32_t count = 0;

    if (!fs::exists("/dev/dri")) {
      BOOST_LOG(warning) << "Couldn't find /dev/dri, kmsgrab won't be enabled"sv;
      return {};
    }

    if (!gbm::create_device) {
      BOOST_LOG(warning) << "libgbm not initialized"sv;
      return {};
    }

    std::vector<kms::card_descriptor_t> cds;

    fs::path card_dir {"/dev/dri"sv};
    for (auto &entry : fs::directory_iterator {card_dir}) {
      auto file = entry.path().filename();

      auto filestring = file.generic_string();
      if (std::string_view {filestring}.substr(0, 4) != "card"sv) {
        continue;
      }

      kms::card_t card;
      if (card.init(entry.path().c_str())) {
        continue;
      }

      // Skip cards whose scanout buffers are not known to support the CUDA
      // import path unless NVENC was explicitly selected by the user.
      if (hwdevice_type == mem_type_e::cuda && !card.supports_cuda_import()) {
        BOOST_LOG(debug) << file << " does not support CUDA framebuffer import"sv;
        if (config::video.encoder == "nvenc" || config::video.encoder == "nvenc_experimental") {
          BOOST_LOG(warning) << "Using NVENC with your display connected to a different GPU may not work properly!"sv;
        } else {
          continue;
        }
      }

      // Skip Nvidia cards if we're looking for VAAPI devices
      // This is important for hybrid GPU laptops where the display
      // may be connected through NVIDIA but rendering happens on Intel
      if (hwdevice_type == mem_type_e::vaapi && card.is_nvidia()) {
        BOOST_LOG(debug) << file << " is an NVIDIA card, skipping for VAAPI"sv;
        continue;
      }

      auto crtc_to_monitor = kms::map_crtc_to_monitor(card.monitors());

      auto end = std::end(card);
      for (auto plane = std::begin(card); plane != end; ++plane) {
        // Skip unused planes
        if (!plane->fb_id) {
          continue;
        }

        if (card.is_cursor(plane->plane_id)) {
          continue;
        }

        auto fb = card.fb(plane.get());
        if (!fb) {
          BOOST_LOG(error) << "Couldn't get drm fb for plane ["sv << plane->fb_id << "]: "sv << strerror(errno);
          continue;
        }

        if (!fb->handles[0]) {
          BOOST_LOG(error) << "Couldn't get handle for DRM Framebuffer ["sv << plane->fb_id << "]: Probably not permitted"sv;
#if defined(SUNSHINE_BUILD_FLATPAK) || defined(SUNSHINE_BUILD_APPIMAGE)
          BOOST_LOG((config::video.capture == "kms") ? fatal : error)
            << "AppImage and Flatpak do not support KMS capture. Use another capture method."sv;
#endif
          break;
        }

        // This appears to return the offset of the monitor
        auto crtc = card.crtc(plane->crtc_id);
        if (!crtc) {
          BOOST_LOG(error) << "Couldn't get CRTC info: "sv << strerror(errno);
          continue;
        }

        auto it = crtc_to_monitor.find(plane->crtc_id);
        if (it != std::end(crtc_to_monitor)) {
          it->second.viewport = platf::touch_port_t {
            (int) crtc->x,
            (int) crtc->y,
            (int) crtc->width,
            (int) crtc->height,
          };
          if (!it->second.monitor_index) {
            it->second.monitor_index = count;
          }
        }

        kms::env_width = std::max(kms::env_width, (int) (crtc->x + crtc->width));
        kms::env_height = std::max(kms::env_height, (int) (crtc->y + crtc->height));

        kms::print(plane.get(), fb.get(), crtc.get());

        ++count;
      }

      cds.emplace_back(kms::card_descriptor_t {
        std::move(file),
        std::move(crtc_to_monitor),
      });
    }

    if (!wl::init()) {
      correlate_to_wayland(cds);
    }

    // Deduce the full virtual desktop size
    kms::env_width = 0;
    kms::env_height = 0;

    kms::env_logical_width = 0;
    kms::env_logical_height = 0;

    for (auto &card_descriptor : cds) {
      for (auto &[_, monitor_descriptor] : card_descriptor.crtc_to_monitor) {
        if (!monitor_descriptor.monitor_index) {
          continue;
        }
        BOOST_LOG(debug) << "Monitor description"sv;
        BOOST_LOG(debug) << "Resolution: "sv << monitor_descriptor.viewport.width << 'x' << monitor_descriptor.viewport.height;
        BOOST_LOG(debug) << "Offset: "sv << monitor_descriptor.viewport.offset_x << 'x' << monitor_descriptor.viewport.offset_y;

        kms::env_width = std::max(kms::env_width, (int) (monitor_descriptor.viewport.offset_x + monitor_descriptor.viewport.width));
        kms::env_height = std::max(kms::env_height, (int) (monitor_descriptor.viewport.offset_y + monitor_descriptor.viewport.height));

        kms::env_logical_height = std::max(kms::env_logical_height, (int) (monitor_descriptor.viewport.offset_y + monitor_descriptor.viewport.logical_height));
        kms::env_logical_width = std::max(kms::env_logical_width, (int) (monitor_descriptor.viewport.offset_x + monitor_descriptor.viewport.logical_width));
      }
    }

    BOOST_LOG(debug) << "Desktop resolution: "sv << kms::env_width << 'x' << kms::env_height;

    std::vector<kms::selection::monitor_t> active_monitors;
    for (const auto &card_descriptor : cds) {
      for (const auto &[crtc_id, monitor_descriptor] : card_descriptor.crtc_to_monitor) {
        if (!monitor_descriptor.monitor_index) {
          continue;
        }
        active_monitors.emplace_back(kms::selection::monitor_t {
          card_descriptor.path,
          monitor_descriptor.connector_name,
          crtc_id,
          *monitor_descriptor.monitor_index,
        });
      }
    }

    auto named_monitors = kms::selection::name_monitors(std::move(active_monitors));

    // The generic capture loop selects configured outputs by comparing them to
    // this name list. Keep an old numeric configuration working without
    // publishing duplicate numeric entries: move that monitor's stable name to
    // the default position for this enumeration.
    if (const auto numeric_alias = kms::selection::parse_numeric_alias(config::get_active_output_name()); numeric_alias) {
      const auto selected = std::ranges::find(named_monitors, *numeric_alias, [](const auto &named_monitor) {
        return named_monitor.monitor.monitor_index;
      });
      if (selected != named_monitors.end()) {
        std::rotate(named_monitors.begin(), selected, std::next(selected));
      }
    }

    std::vector<std::string> display_names;
    display_names.reserve(named_monitors.size());
    for (const auto &named_monitor : named_monitors) {
      display_names.emplace_back(named_monitor.display_name);
    }

    kms::card_descriptors = std::move(cds);
    kms::named_monitors = std::move(named_monitors);

    return display_names;
  }

}  // namespace platf
