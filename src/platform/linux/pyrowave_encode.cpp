/** @file src/platform/linux/pyrowave_encode.cpp
 * @brief Linux capture adapter for the Vulkan PyroWave encoder.
 */
#include "graphics.h"
#include "pyrowave_core.h"
#include "src/platform/common.h"
#include "src/pyrowave_host.h"
#include "src/pyrowave_protocol.h"

#include <cstring>
#include <limits>

namespace pyrowave::host {
  namespace {
    linux_gpu::config_t core_config(const session_params_t &p) {
      linux_gpu::config_t c;
      c.width = p.width;
      c.height = p.height;
      c.yuv444 = p.yuv444;
      c.ten_bit = p.colorspace.bit_depth > 8;
      c.hdr = video::colorspace_is_hdr(p.colorspace);
      if (auto *matrix = video::color_vectors_from_colorspace(p.colorspace, true)) {
        std::memcpy(c.matrix.data(), matrix, sizeof(float) * 12);
      }
      c.render_device = platf::resolve_render_device();
      return c;
    }

    class encoder_impl_t final: public encoder_t {
    public:
      encoder_impl_t(const session_params_t &p, std::shared_ptr<platf::display_t> d):
          params(p),
          display(std::move(d)),
          budget(p.framerate, p.bitrate_kbps, policy::max_bitstream_bytes(p.packetsize, p.framing == policy::framing_e::length_prefixed, p.critical_fec),
                 p.critical_fec && p.framing == policy::framing_e::records) {}

      void set_bitrate(int bitrate) override {
        budget.set_bitrate(bitrate);
      }

      void on_frame(std::chrono::steady_clock::time_point when) override {
        budget.on_frame(when);
      }

      int encode(platf::img_t &image, std::vector<std::uint8_t> &out, std::size_t &critical_bytes) override {
        out.clear();
        critical_bytes = 0;
        if (budget.bytes_per_frame() < 16) {
          return 1;
        }
        linux_gpu::source_t source;
        std::string capture_device;
        if (auto *img = dynamic_cast<egl::img_descriptor_t *>(&image)) {
          if (!img->sequence || img->sd.fds[0] < 0) {
            return 1;
          }
          source.surface = &img->sd;
          source.width = display->width;
          source.height = display->height;
          source.offset_x = img->capture_offset_x;
          source.offset_y = img->capture_offset_y;
          source.y_invert = img->y_invert;
          source.cursor = img->data;
          source.cursor_width = img->src_w;
          source.cursor_height = img->src_h;
          source.cursor_x = img->x;
          source.cursor_y = img->y;
          source.cursor_dst_width = img->width;
          source.cursor_dst_height = img->height;
          source.lut = img->crtc_gamma_lut.get();
          capture_device = img->capture_render_device;
        } else {
          if (!image.data) {
            return 1;
          }
          if (image.pixel_pitch != 4) {
            BOOST_LOG(error) << "PyroWave: unsupported RAM capture format";
            return -1;
          }
          source.pixels = image.data;
          source.width = image.width;
          source.height = image.height;
          source.stride = image.row_pitch;
        }
        std::string detail;
        if (!core) {
          auto config = core_config(params);
          if (!capture_device.empty()) {
            config.render_device = capture_device;
          }
          core = linux_gpu::core_t::create(config, detail);
          if (!core) {
            BOOST_LOG(error) << "PyroWave: " << detail;
            return -1;
          }
          BOOST_LOG(info) << "PyroWave encoder ready (Linux Vulkan)";
        }
        bool records = params.framing == policy::framing_e::records;
        auto boundary = records ? std::size_t(UINT32_MAX) : protocol::LENGTH_PREFIXED_PACKET_BOUNDARY;
        if (!core->encode(source, budget.bytes_per_frame(), boundary, scratch, packets, detail)) {
          BOOST_LOG(error) << "PyroWave: " << detail;
          return -1;
        }
        if (records) {
          if (packets.size() != 1) {
            return -1;
          }
          auto &p = packets.front();
          auto stats = policy::write_record_frame(std::span<const std::uint8_t>(scratch.data() + p.offset, p.size), policy::shard_payload_bytes(params.packetsize), out, policy::max_frame_bytes(params.packetsize, params.critical_fec));
          if (!stats) {
            return -1;
          }
          critical_bytes = stats->critical_bytes;
        } else {
          std::vector<policy::packet_t> layout;
          for (auto &p : packets) {
            layout.push_back({p.offset, p.size});
          }
          policy::write_length_prefixed_frame(layout, scratch.data(), out);
        }
        if (out.size() > policy::max_frame_bytes(params.packetsize, params.critical_fec)) {
          out.clear();
          critical_bytes = 0;
          return 1;
        }
        return 0;
      }

    private:
      session_params_t params;
      std::shared_ptr<platf::display_t> display;
      policy::budget_t budget;
      std::unique_ptr<linux_gpu::core_t> core;
      std::vector<std::uint8_t> scratch;
      std::vector<linux_gpu::packet_t> packets;
    };
  }  // namespace

  std::unique_ptr<encoder_t> make_encoder(const session_params_t &p, std::shared_ptr<platf::display_t> d) {
    if (!d || p.width <= 0 || p.height <= 0 || (!p.yuv444 && ((p.width | p.height) & 1)) || p.framerate <= 0 || p.bitrate_kbps <= 0 || !policy::max_frame_bytes(p.packetsize)) {
      return {};
    }
    return std::make_unique<encoder_impl_t>(p, std::move(d));
  }

  bool probe(const std::optional<platf::adapter_id_t> &, std::string &detail) {
    if (!platf::pyrowave_capture_supported()) {
      detail = "CUDA-only NvFBC capture is not supported by PyroWave; use KMS or DMA-BUF capture";
      return false;
    }
    // Execute conversion, encode and readback, rather than only creating a device.
    session_params_t p;
    p.width = p.height = 128;
    p.colorspace = {video::colorspace_e::rec709, false, 8};
    auto core = linux_gpu::core_t::create(core_config(p), detail);
    if (!core) {
      return false;
    }
    std::vector<std::uint8_t> pixels(128 * 128 * 4, 128), bits;
    std::vector<linux_gpu::packet_t> packets;
    linux_gpu::source_t source;
    source.pixels = pixels.data();
    source.width = source.height = 128;
    source.stride = 512;
    return core->encode(source, 65536, UINT32_MAX, bits, packets, detail);
  }
}  // namespace pyrowave::host
