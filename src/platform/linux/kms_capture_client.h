/** @file Unprivileged client for the restricted SteamOS virtual-KMS helper. */
#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <sys/types.h>
#include <vibeshine_drm_uapi.h>
#include <xf86drmMode.h>

#include "kms_capture_protocol.h"

namespace platf::kms_capture {
  class client_t final {
  public:
    /** Bind the helper to the character device identified by this open fd. */
    static std::unique_ptr<client_t> open(int ordinary_card_fd);
    ~client_t();
    client_t(const client_t &) = delete;
    client_t &operator=(const client_t &) = delete;

    /** Returned descriptors belong to the caller. Metadata handles are zero. */
    int framebuffer(uint32_t plane_id, uint32_t crtc_id, uint32_t fb_id,
                    drmModeFB2 &metadata, std::array<int, 4> &dma_buf_fds);
    int wait(vibeshine_drm_wait_present &request);
    int frame(vibeshine_drm_frame &request);

  private:
    friend struct client_test_access;
    client_t(int socket, pid_t child): socket_ {socket}, child_ {child} {}
    int transact(const vibeshine_kms_capture_request &request,
                 vibeshine_kms_capture_response &response);
    void disconnect() noexcept;
    int socket_ {-1};
    pid_t child_ {-1};
    std::mutex mutex_;
  };
}  // namespace platf::kms_capture
