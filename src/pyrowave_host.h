/**
 * @file src/pyrowave_host.h
 * @brief Platform-neutral interface of the PyroWave host encoder.
 *
 * PyroWave (https://github.com/Themaister/pyrowave) is an intra-only GPU wavelet
 * codec. It does not ride the avcodec/NVENC/AMF encoder_t abstraction: a session
 * whose client negotiated bitStreamFormat 3 is encoded here whatever hardware
 * encoder was probed, and every frame is sent as an IDR frame. See
 * docs/pyrowave-protocol.md.
 */
#pragma once

#include "pyrowave_policy.h"
#include "video_colorspace.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace platf {
  struct img_t;
  class display_t;
  struct adapter_id_t;
}  // namespace platf

namespace pyrowave::host {
  struct session_params_t {
    int width = 0;
    int height = 0;
    bool yuv444 = false;
    video::sunshine_colorspace_t colorspace {};
    int framerate = 0;
    int bitrate_kbps = 0;  ///< Adjusted encoder bitrate (audio and control overhead removed).
    policy::framing_e framing = policy::framing_e::records;
    int packetsize = 0;  ///< Negotiated RTP packet size, for record alignment.
    bool critical_fec = false;  ///< stream.cpp adds an FEC block for the critical shards.
  };

  class encoder_t {
  public:
    virtual ~encoder_t() = default;

    /**
     * @brief Encode one captured image, replacing `out` with the framed frame.
     * @param critical_bytes Receives the frame bytes through the coarsest wavelet level
     *        (record framing), or 0 when the framing has no such prefix.
     * @return 0 on success; negative on a fatal error (end the session); positive
     *         when this frame was skipped but the session can continue.
     */
    virtual int encode(platf::img_t &img, std::vector<std::uint8_t> &out, std::size_t &critical_bytes) = 0;

    /// Apply a new encoder bitrate (dynamic bitrate); only the per-frame budget changes.
    virtual void set_bitrate(int bitrate_kbps) = 0;

    /// Account for every frame submitted for encoding, including repeats.
    virtual void on_frame(std::chrono::steady_clock::time_point when) = 0;
  };

  /**
   * @brief Create the platform encoder for a session.
   * @param display The capture display the images come from.
   * @return nullptr when PyroWave encoding is unavailable on this platform or build.
   */
  std::unique_ptr<encoder_t> make_encoder(const session_params_t &params, std::shared_ptr<platf::display_t> display);

  /**
   * @brief Whether this build can encode PyroWave on the given capture adapter.
   *
   * Creates (and destroys) a GPU device, so call it from encoder probing, not per frame.
   * @param detail Receives the reason when unsupported.
   */
  bool probe(const std::optional<platf::adapter_id_t> &adapter, std::string &detail);
}  // namespace pyrowave::host
