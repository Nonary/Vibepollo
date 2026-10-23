#pragma once

#include "display_vram.h"
#include "frame_push_d3d11.h"
#include "frame_push_windows_io.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>

namespace platf::dxgi {
  class display_frame_push_vram_t final: public display_vram_t {
  public:
    ~display_frame_push_vram_t() override;
    int init(const ::video::config_t &config, const std::string &display_name,
             const std::optional<LUID> &required_adapter_luid = std::nullopt);
    void request_capture_stop() noexcept override;
    void prepare_for_reinit() override;

  protected:
    capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb,
                       std::shared_ptr<platf::img_t> &img_out,
                       std::chrono::milliseconds timeout,
                       bool cursor_visible) override;
    capture_e release_snapshot() override;

  private:
    void stop_session();

    std::atomic<bool> stop_requested_ {false};
    std::shared_ptr<frame_push::AsyncIo> io_ {};
    std::unique_ptr<frame_push::BoundedTransactionTransport> transport_ {};
    std::unique_ptr<frame_push::Session> session_ {};
    std::optional<frame_push::Frame> first_frame_ {};
    std::chrono::steady_clock::time_point last_frame_at_ {};
  };
}
