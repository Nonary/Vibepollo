#include "display_frame_push.h"

#include "misc.h"
#include "src/logging.h"
#include "frame_push_capture_policy.h"
#include "virtual_display.h"

#include <algorithm>
#include <chrono>
#include <memory>

namespace platf::dxgi {
  namespace {
    constexpr std::uint32_t kCaptureMutexTimeoutMs = 3000;
    constexpr std::uint32_t kMaxFramePollMs = 100;

    DXGI_FORMAT dxgi_format(const std::uint32_t format) {
      return static_cast<DXGI_FORMAT>(format);
    }
  }

  display_frame_push_vram_t::~display_frame_push_vram_t() {
    stop_session();
  }

  int display_frame_push_vram_t::init(
    const ::video::config_t &config,
    const std::string &display_name,
    const std::optional<LUID> &required_adapter_luid
  ) {
    stop_requested_.store(false, std::memory_order_release);
    if (display_base_t::init(config, display_name, true, required_adapter_luid) != 0) return -1;

    const auto target = VDISPLAY::frame_push_target_for_output(display_name);
    if (!target || !target->lease_id || !target->display_id || !target->display_generation ||
        !target->width || !target->height || target->control_device_path.empty()) {
      BOOST_LOG(debug) << "Frame-push initialization rejected: no owner-authenticated live target was returned.";
      return -1;
    }
    if (!frame_push::policy::target_matches_capture_output(
          target->width, target->height, target->adapter_luid_low_part, target->adapter_luid_high_part,
          static_cast<std::uint32_t>(width_before_rotation), static_cast<std::uint32_t>(height_before_rotation),
          captured_adapter_luid.LowPart, captured_adapter_luid.HighPart)) {
      BOOST_LOG(debug) << "Frame-push initialization rejected: active IddCx target does not match the selected DXGI output mode/adapter.";
      return -1;
    }
    if (!frame_push::policy::supports_capture_color_mode(
          target->format, target->color_space, captured_hdr_state_valid,
          static_cast<std::uint32_t>(captured_color_desc.ColorSpace))) {
      BOOST_LOG(debug) << "Frame-push initialization rejected: only verified SDR P709 capture is currently supported.";
      return -1;
    }

    capture_format = dxgi_format(target->format);
    Microsoft::WRL::ComPtr<ID3D11Device1> device1;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))) || !device_ctx ||
        device_ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
      BOOST_LOG(debug) << "Frame-push initialization rejected: capture device has no immediate D3D11.1 context.";
      return -1;
    }

    frame_push::vdd::FrameRingRegisterRequest request {};
    request.lease_id = target->lease_id;
    request.display_id = target->display_id;
    request.display_generation = target->display_generation;
    request.owner_capability.bytes = target->owner_capability;
    request.adapter_luid = {target->adapter_luid_low_part, target->adapter_luid_high_part};
    request.width = target->width;
    request.height = target->height;
    request.format = target->format;
    request.color_space = target->color_space;

    auto resources = frame_push::create_d3d11_consumer_ring(device1.Get(), device_ctx.get(), request);
    if (!resources) {
      BOOST_LOG(debug) << "Frame-push initialization rejected: failed to allocate a matching four-slot consumer ring.";
      return -1;
    }
    io_ = frame_push::open_frame_push_windows_io(target->control_device_path);
    if (!io_) {
      BOOST_LOG(debug) << "Frame-push initialization rejected: unable to open the selected driver control endpoint.";
      return -1;
    }
    transport_ = std::make_unique<frame_push::BoundedTransactionTransport>(io_);
    session_ = std::make_unique<frame_push::Session>(*transport_);
    const auto status = session_->start(std::move(resources));
    if (status != frame_push::Status::Ready) {
      BOOST_LOG(debug) << "Frame-push registration did not become ready (status=" << static_cast<int>(status) << "); using WGC fallback.";
      stop_session();
      return -1;
    }
    auto first_frame = frame_push::wait_for_first_frame(*session_, 2000);
    if (first_frame.status != frame_push::Status::Ready || !first_frame.pixels) {
      BOOST_LOG(debug) << "Frame-push producer did not deliver a valid first frame (status="
                       << static_cast<int>(first_frame.status) << "); using WGC fallback.";
      stop_session();
      return -1;
    }
    first_frame_ = std::move(first_frame);
    last_frame_at_ = std::chrono::steady_clock::now();
    BOOST_LOG(info) << "Frame-push capture adapter bound to display " << target->display_id
                    << " generation " << target->display_generation << " on "
                    << target->width << 'x' << target->height << '.';
    return 0;
  }

  void display_frame_push_vram_t::request_capture_stop() noexcept {
    stop_requested_.store(true, std::memory_order_release);
  }

  void display_frame_push_vram_t::stop_session() {
    request_capture_stop();
    first_frame_.reset();
    last_frame_at_ = {};
    if (session_) {
      session_->stop();
      session_.reset();
    }
    transport_.reset();
    io_.reset();
  }

  void display_frame_push_vram_t::prepare_for_reinit() {
    stop_session();
    display_base_t::prepare_for_reinit();
  }

  capture_e display_frame_push_vram_t::snapshot(
    const pull_free_image_cb_t &pull_free_image_cb,
    std::shared_ptr<platf::img_t> &img_out,
    const std::chrono::milliseconds timeout,
    const bool
  ) {
    if (stop_requested_.load(std::memory_order_acquire)) return capture_e::interrupted;
    if (!session_ || !session_->active()) return capture_e::reinit;
    const auto poll_ms = static_cast<std::uint32_t>((std::clamp<std::int64_t>)(timeout.count(), 0, kMaxFramePollMs));
    auto frame = first_frame_ ? std::move(*first_frame_) : session_->next(poll_ms);
    first_frame_.reset();
    if (stop_requested_.load(std::memory_order_acquire)) return capture_e::interrupted;
    switch (frame.status) {
      case frame_push::Status::Timeout:
      case frame_push::Status::NoFrame:
        if (last_frame_at_ != std::chrono::steady_clock::time_point {} &&
            frame_push::policy::producer_stalled(std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - last_frame_at_
            ))) {
          BOOST_LOG(warning) << "Frame-push producer stopped publishing; restarting capture through the selector.";
          stop_session();
          return capture_e::reinit;
        }
        return capture_e::timeout;
      case frame_push::Status::Stopped:
        return capture_e::interrupted;
      case frame_push::Status::DeviceLost:
      case frame_push::Status::InvalidFrame:
      case frame_push::Status::TransportError:
      case frame_push::Status::InvalidReply:
      case frame_push::Status::Unsupported:
      case frame_push::Status::InvalidTarget:
        return capture_e::reinit;
      case frame_push::Status::Ready:
        last_frame_at_ = std::chrono::steady_clock::now();
        break;
    }
    if (!frame.pixels) return capture_e::reinit;
    const auto pixels = std::dynamic_pointer_cast<frame_push::D3D11PrivateFrame>(frame.pixels);
    if (!pixels || !pixels->texture) return capture_e::reinit;

    if (!pull_free_image_cb(img_out)) return capture_e::interrupted;
    const auto image = std::static_pointer_cast<img_d3d_t>(img_out);
    if (!image || complete_img(image.get(), false) != 0) return capture_e::error;
    if (!image->capture_mutex || !image->capture_texture || image->format != capture_format ||
        image->width != width_before_rotation || image->height != height_before_rotation) return capture_e::reinit;

    const HRESULT acquired = image->capture_mutex->AcquireSync(0, kCaptureMutexTimeoutMs);
    if (acquired != S_OK && acquired != WAIT_ABANDONED) {
      BOOST_LOG(error) << "Frame-push could not acquire the encoder image mutex [0x"sv
                       << util::hex(acquired).to_string_view() << ']';
      return capture_e::error;
    }
    auto release_capture_mutex = util::fail_guard([&]() {
      const HRESULT released = image->capture_mutex->ReleaseSync(0);
      if (FAILED(released)) {
        BOOST_LOG(warning) << "Frame-push failed to release the encoder image mutex [0x"sv
                           << util::hex(released).to_string_view() << ']';
      }
    });

    D3D11_TEXTURE2D_DESC source_desc {};
    pixels->texture->GetDesc(&source_desc);
    if (source_desc.Width != image->width || source_desc.Height != image->height ||
        source_desc.Format != image->format || source_desc.SampleDesc.Count != 1) return capture_e::reinit;

    const auto host_processing_timestamp = std::chrono::steady_clock::now();
    const auto host_processing_qpc = qpc_counter();
    device_ctx->CopyResource(image->capture_texture.get(), pixels->texture.Get());
    device_ctx->Flush();
    if (FAILED(device->GetDeviceRemovedReason())) return capture_e::reinit;
    image->blank = false;
    const auto frame_timestamp = host_processing_timestamp -
      qpc_time_difference(host_processing_qpc, static_cast<std::int64_t>(frame.metadata.acquired_qpc));
    img_out->frame_timestamp = frame_timestamp;
    img_out->host_processing_timestamp = host_processing_timestamp;
    img_out->capture_pacing_timestamp = host_processing_timestamp;
    return capture_e::ok;
  }

  capture_e display_frame_push_vram_t::release_snapshot() {
    // Session::next() releases the producer keyed mutex after making its private
    // copy; this display adapter never returns a live shared-ring slot.
    return capture_e::ok;
  }
}
