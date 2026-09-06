// SPDX-License-Identifier: GPL-3.0-only
// Links the product objects; run against hdr-pattern in an isolated Gamescope.
#include "src/config.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/linux/gamescopegrab.h"
#include "src/platform/linux/graphics.h"
#include "src/platform/linux/display_backend.h"
#include "src/rtsp.h"
#include "src/video.h"

#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

namespace video {
  struct hdr_latch_t;
  extern encoder_t vaapi;
  extern encoder_t vulkan;
  std::unique_ptr<platf::encode_device_t> make_encode_device(platf::display_t &, const encoder_t &, const config_t &, hdr_latch_t *, bool);
  std::unique_ptr<encode_session_t> make_encode_session(platf::display_t *, const encoder_t &, const config_t &, int, int, std::unique_ptr<platf::encode_device_t>, std::chrono::steady_clock::time_point, std::function<bool()>, bool *, bool *);
  int encode(int64_t, encode_session_t &, safe::mail_raw_t::queue_t<packet_t> &, void *, std::optional<std::chrono::steady_clock::time_point>, std::optional<std::chrono::steady_clock::time_point>);
}  // namespace video

static unsigned pq(double nits) {
  double p = std::pow(nits / 10000.0, 2610.0 / 16384.0);
  return std::lround(1023.0 * std::pow((3424.0 / 4096.0 + (2413.0 / 128.0) * p) / (1.0 + (2392.0 / 128.0) * p), 2523.0 / 32.0));
}

int main(int argc, char **argv) {
  const bool stock = argc > 1 && std::string_view(argv[1]) == "--stock";
  const bool sdr_source = argc > 1 && std::string_view(argv[1]) == "--sdr-source";
  auto log = logging::init(2, "/tmp/vibepollo-hdr-capture-probe.log");
  mail::man = std::make_shared<safe::mail_raw_t>();
  config::video.capture.clear();
  const bool use_vulkan = std::getenv("VIBEPOLLO_HDR_ENCODER") && std::string_view(std::getenv("VIBEPOLLO_HDR_ENCODER")) == "vulkan";
  config::video.encoder = use_vulkan ? "vulkan" : "vaapi";
  const auto memory_type = use_vulkan ? platf::mem_type_e::vulkan : platf::mem_type_e::vaapi;
  const auto &encoder = use_vulkan ? video::vulkan : video::vaapi;
  config::video.virtual_display_mode = config::video_t::virtual_display_mode_e::per_client;
  config::video.output_name = "Virtual-1";
  auto platform = platf::init();
  if (!platform || !platf::gamescope_capture_selected()) {
    return 2;
  }
  // Exercise the real launch/resume preparation with Desktop Mode preferences
  // still configured. No KScreen broker, connector lease, or SDR override is
  // required for the Gamescope scene, on stock or patched compositors.
  using mode_e = config::video_t::virtual_display_mode_e;
  using layout_e = config::video_t::virtual_display_layout_e;
  for (auto mode : {mode_e::disabled, mode_e::per_client, mode_e::shared}) {
    for (auto layout : {layout_e::exclusive, layout_e::extended_primary}) {
      for (bool resume : {false, true}) {
        config::video.virtual_display_mode = mode;
        config::video.virtual_display_layout = layout;
        rtsp_stream::launch_session_t session {};
        session.enable_hdr = true;
        session.virtual_display = resume;
        session.virtual_display_failed = resume;
        session.virtual_display_device_id = "Virtual-1";
        session.virtual_display_hdr_enabled = false;
        session.virtual_display_recreated_on_demand = resume;
        session.virtual_display_needs_resume_apply = resume;
        session.client_requests_virtual_display = mode != mode_e::disabled;
        session.client_virtual_display_override = mode != mode_e::disabled;
        session.output_name_override = "DP-1";
        const auto &backend = platf::linux_display::backend();
        auto prepared = backend.prepare_session(session, !resume, !resume);
        if (prepared.owns_output || !prepared.error.empty() ||
            prepared.output_name != "gamescope" || session.output_name_override != "gamescope" ||
            session.virtual_display || session.virtual_display_failed ||
            !session.virtual_display_device_id.empty() || session.virtual_display_hdr_enabled ||
            session.virtual_display_recreated_on_demand || session.virtual_display_needs_resume_apply ||
            session.virtual_display_mode_override != mode_e::disabled ||
            !rtsp_stream::effective_hdr_requested(session) ||
            config::video.virtual_display_mode != mode || config::video.virtual_display_layout != layout ||
            config::video.output_name != "Virtual-1" ||
            !backend.initialize() || !backend.apply_session(session) || !backend.revert() ||
            !backend.reset_persistence() || backend.capabilities().independent_outputs ||
            backend.capture_target("Virtual-1") != "gamescope") {
          std::cerr << "Gamescope launch/resume routing failed\n";
          return 20;
        }
      }
    }
  }
  std::cout << "Gamescope automatic capture and 12 launch/resume display-policy cases PASS\n";
  video::config_t requested {};
  requested.width = 1280;
  requested.height = 800;
  requested.framerate = 30;
  requested.dynamicRange = 1;
  requested.videoFormat = 1;
  auto display = platf::display(memory_type, "Virtual-1", requested);
  if (stock) {
    if (display) {
      return 3;
    }
    requested.dynamicRange = 0;
    display = platf::display(memory_type, "DP-1", requested);
    if (!display || display->is_hdr()) {
      return 4;
    }
    std::cout << "Stock Gamescope: HDR rejected; SDR capture available\n";
    return 0;
  }
  if (!display || !display->is_hdr() || display->width != 1280 || display->height != 800) {
    return 5;
  }
  SS_HDR_METADATA metadata {};
  if (!display->get_hdr_metadata(metadata) || metadata.maxDisplayLuminance != 10000 || metadata.maxContentLightLevel != 0) {
    return 6;
  }
  unsigned levels[] = {0, pq(100), pq(203), pq(1000), pq(4000), pq(1000), pq(1000), pq(1000)};
  std::array<std::array<unsigned, 3>, 8> actual {};
  int frames = 0;
  bool valid = false, cursor = false;
  std::shared_ptr<platf::img_t> retained;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
  auto status = display->capture([&](std::shared_ptr<platf::img_t> &&image, bool captured) {
    if (captured) {
      auto *desc = static_cast<egl::img_descriptor_t *>(image.get());
      if (desc->sd.fds[0] < 0) {
        return false;
      }
      size_t size = desc->sd.offsets[0] + desc->sd.pitches[0] * 800u;
      dma_buf_sync sync {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
      if (ioctl(desc->sd.fds[0], DMA_BUF_IOCTL_SYNC, &sync) < 0) {
        return false;
      }
      auto *mapped = static_cast<const unsigned char *>(mmap(nullptr, size, PROT_READ, MAP_SHARED, desc->sd.fds[0], 0));
      if (mapped == MAP_FAILED) {
        return false;
      }
      valid = true;
      for (unsigned band = 0; band < 8; ++band) {
        auto pixel = *reinterpret_cast<const uint32_t *>(mapped + desc->sd.offsets[0] + 400 * desc->sd.pitches[0] + (band * 160 + 80) * 4);
        for (unsigned channel = 0; channel < 3; ++channel) {
          actual[band][channel] = (pixel >> (channel * 10)) & 1023;
          unsigned expected = sdr_source ? (band == 0 ? 0 : pq(203)) : levels[band];
          if (band >= 5 && channel != band - 5) {
            expected = 0;
          }
          // SDR primaries are gamut-converted from 709 into 2020, so only
          // validate neutral SDR patches; HDR primaries must stay in place.
          if ((!sdr_source || band < 5) && std::abs(int(actual[band][channel]) - int(expected)) > 8) {
            valid = false;
          }
        }
      }
      munmap(const_cast<unsigned char *>(mapped), size);
      sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
      ioctl(desc->sd.fds[0], DMA_BUF_IOCTL_SYNC, &sync);
      retained = image;
      ++frames;
    }
    return frames < 12 && std::chrono::steady_clock::now() < deadline;
  },
                                 [&](std::shared_ptr<platf::img_t> &image) {
                                   if (std::chrono::steady_clock::now() >= deadline) {
                                     return false;
                                   }
                                   image = display->alloc_img();
                                   return true;
                                 },
                                 &cursor);
  std::cout << "HDR frames=" << frames << " status=" << int(status) << " pixels_match=" << valid << '\n';
  for (auto band : actual) {
    std::cout << band[0] << ',' << band[1] << ',' << band[2] << ' ';
  }
  std::cout << std::endl;
  if (frames < 12 || !valid) {
    return 7;
  }
  if (std::getenv("VIBEPOLLO_HDR_CAPTURE_ONLY")) {
    std::cout << "Capture and reference-volume metadata PASS (encoding not requested)\n";
    return 0;
  }
  display.reset();
  int result = video::probe_encoders();
  std::cout << config::video.encoder << " probe=" << result << " HEVC mode=" << video::active_hevc_mode << std::endl;
  if (result != 0 || video::active_hevc_mode != 3) {
    return 8;
  }
  // Encode the actual captured PQ pattern through the product's GPU converter,
  // codec, and packet replacements, rather than only its black-frame probe.
  display = platf::gamescope_display(memory_type, "gamescope", requested);
  if (!display) {
    return 9;
  }
  requested.bitrate = 20000;
  requested.slicesPerFrame = 1;
  requested.numRefFrames = 1;
  requested.encoderCscMode = std::getenv("VIBEPOLLO_HDR_FULL_RANGE") ? 1 : 0;
  auto device = video::make_encode_device(*display, encoder, requested, nullptr, false);
  if (!device) {
    return 10;
  }
  auto session = video::make_encode_session(display.get(), encoder, requested, 1280, 800, std::move(device), std::chrono::steady_clock::now() + std::chrono::seconds(5), {}, nullptr, nullptr);
  if (!session) {
    return 11;
  }
  auto packets = mail::man->queue<video::packet_t>(mail::video_packets);
  const char *output = std::getenv("VIBEPOLLO_HDR_BITSTREAM");
  std::ofstream bitstream(output ? output : "/tmp/vibepollo-hdr-pattern.hevc", std::ios::binary);
  if (!bitstream) {
    return 12;
  }
  int encoded = 0;
  session->request_idr_frame();
  for (int frame = 1; frame <= 16; ++frame) {
    if (session->convert(*retained) || video::encode(frame, *session, packets, nullptr, {}, {})) {
      return 13;
    }
    session->request_normal_frame();
    while (packets->peek()) {
      auto packet = packets->pop();
      std::string payload(reinterpret_cast<char *>(packet->data()), packet->data_size());
      if (packet->is_idr() && packet->replacements) {
        for (const auto &replacement : *packet->replacements) {
          if (replacement.old.empty()) {
            continue;
          }
          auto position = payload.find(replacement.old);
          if (position != std::string::npos) {
            payload.replace(position, replacement.old.size(), replacement._new);
          }
        }
      }
      bitstream.write(payload.data(), payload.size());
      ++encoded;
    }
  }
  std::cout << "Encoded captured HDR pattern packets=" << encoded << std::endl;
  return encoded >= 12 && bitstream.good() ? 0 : 14;
}
