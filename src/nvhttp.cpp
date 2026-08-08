/**
 * @file src/nvhttp.cpp
 * @brief Definitions for the nvhttp (GameStream) server.
 */
// macros
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <Simple-Web-Server/server_http.hpp>

// local includes
#include "config.h"
#include "display_device.h"
#include "display_helper_integration.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "state_storage.h"
#include "update.h"
#ifdef _WIN32
  #include "platform/windows/display.h"
  #include "platform/windows/display_helper_request_policy.h"
  #include "platform/windows/display_helper_request_helpers.h"
  #include "platform/windows/misc.h"
  #include "platform/windows/virtual_display.h"
  #include "platform/windows/virtual_display_cleanup.h"
#endif
#include "process.h"
#include "rtsp.h"
#include "stream.h"
#include "system_tray.h"
#include "video.h"
#include "webrtc_stream.h"
#include "zwpad.h"

using namespace std::literals;

namespace nvhttp {

  static constexpr std::string_view EMPTY_PROPERTY_TREE_ERROR_MSG = "Property tree is empty. Probably, control flow got interrupted by an unexpected C++ exception. This is a bug in Sunshine. Moonlight-qt will report Malformed XML (missing root element)."sv;

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  using p_named_cert_t = crypto::p_named_cert_t;
  using PERM = crypto::PERM;
  using verified_client_t = std::optional<crypto::named_cert_t>;

  namespace {
    std::int64_t now_seconds() {
      return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()
      )
        .count();
    }
  }  // namespace

  struct client_t {
    std::vector<p_named_cert_t> named_devices;
  };

  struct pair_session_t;

  crypto::cert_chain_t cert_chain;
  static std::shared_ptr<safe::queue_t<crypto::x509_t>> pending_cert_queue =
    std::make_shared<safe::queue_t<crypto::x509_t>>(30);
  static std::string one_time_pin;
  static std::string otp_passphrase;
  static std::string otp_device_name;
  static std::chrono::time_point<std::chrono::steady_clock> otp_creation_time;
  thread_local crypto::x509_t tl_peer_certificate;

  std::string cert_subject_name_for_log(const crypto::x509_t &cert) {
    auto subject_name = crypto::subject_name(cert.get());
    if (subject_name.empty()) {
      return "unknown"s;
    }
    return subject_name;
  }

  class SunshineHTTPSServer: public SimpleWeb::ServerBase<SunshineHTTPS> {
  public:
    SunshineHTTPSServer(const std::string &certification_file, const std::string &private_key_file):
        ServerBase<SunshineHTTPS>::ServerBase(443),
        context(boost::asio::ssl::context::tls_server) {
      // Disabling TLS 1.0 and 1.1 (see RFC 8996)
      context.set_options(boost::asio::ssl::context::no_tlsv1);
      context.set_options(boost::asio::ssl::context::no_tlsv1_1);
      context.use_certificate_chain_file(certification_file);
      context.use_private_key_file(private_key_file, boost::asio::ssl::context::pem);
    }

    std::function<bool(std::shared_ptr<Request>, SSL *)> verify;
    std::function<void(std::shared_ptr<Response>, std::shared_ptr<Request>)> on_verify_failed;

  protected:
    boost::asio::ssl::context context;

    void after_bind() override {
      if (verify) {
        context.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert | boost::asio::ssl::verify_client_once);
        context.set_verify_callback([](int verified, boost::asio::ssl::verify_context &ctx) {
          // To respond with an error message, a connection must be established
          return 1;
        });
      }
    }

    // This is Server<HTTPS>::accept() with SSL validation support added
    void accept() override {
      auto connection = create_connection(*io_service, context);

      acceptor->async_accept(connection->socket->lowest_layer(), [this, connection](const SimpleWeb::error_code &ec) {
        auto lock = connection->handler_runner->continue_lock();
        if (!lock) {
          return;
        }

        if (ec != SimpleWeb::error::operation_aborted) {
          this->accept();
        }

        auto session = std::make_shared<Session>(config.max_request_streambuf_size, connection);

        if (!ec) {
          boost::asio::ip::tcp::no_delay option(true);
          SimpleWeb::error_code ec;
          session->connection->socket->lowest_layer().set_option(option, ec);

          session->connection->set_timeout(config.timeout_request);
          session->connection->socket->async_handshake(boost::asio::ssl::stream_base::server, [this, session](const SimpleWeb::error_code &ec) {
            session->connection->cancel_timeout();
            auto lock = session->connection->handler_runner->continue_lock();
            if (!lock) {
              return;
            }
            if (!ec) {
              if (verify && !verify(session->request, session->connection->socket->native_handle())) {
                this->write(session, on_verify_failed);
              } else {
                this->read(session);
              }
            } else if (this->on_error) {
              this->on_error(session->request, ec);
            }
          });
        } else if (this->on_error) {
          this->on_error(session->request, ec);
        }
      });
    }
  };

  using https_server_t = SunshineHTTPSServer;
  using http_server_t = SimpleWeb::Server<SimpleWeb::HTTP>;

  struct conf_intern_t {
    std::string servercert;
    std::string pkey;
  } conf_intern;

  struct http_encoder_capabilities_t {
    // Keep verification paired with the advertised values. On Windows, the
    // helper may use a temporary adapter identity that is cleared on return.
    video::advertised_encoder_capabilities_t advertised;
    bool probe_complete;
  };

#ifdef _WIN32
  namespace {
    bool display_helper_session_available() {
      if (platf::is_running_as_system()) {
        return true;
      }
      HANDLE user_token = platf::retrieve_users_token(false);
      const bool available = (user_token != nullptr);
      if (user_token) {
        CloseHandle(user_token);
      }
      return available;
    }

    bool has_active_virtual_display() {
      const auto virtual_displays = VDISPLAY::enumerateVirtualDisplays();
      return std::any_of(
        virtual_displays.begin(),
        virtual_displays.end(),
        [](const VDISPLAY::VirtualDisplayInfo &info) {
          return info.is_active;
        }
      );
    }

    bool has_any_active_display() {
      if (VDISPLAY::has_active_physical_display()) {
        return true;
      }
      if (VDISPLAY::has_retained_ensure_display()) {
        return true;
      }
      return has_active_virtual_display();
    }

    void wait_for_probe_helper_settle(
      const std::shared_ptr<rtsp_stream::launch_session_t> &launch_session,
      const std::chrono::steady_clock::time_point deadline
    ) {
      if (!launch_session->display_helper_gate.valid()) {
        return;
      }
      if (launch_session->display_helper_gate.wait_until(deadline) != std::future_status::ready) {
        BOOST_LOG(warning) << "Display-helper verification did not finish before encoder probing; proceeding on the selected adapter.";
        return;
      }

      try {
        const auto status = launch_session->display_helper_gate.get();
        if (status == rtsp_stream::launch_session_t::display_helper_gate_status_e::abort_failed) {
          BOOST_LOG(warning) << "Display-helper verification failed; proceeding with GPU capability probing.";
        } else if (status == rtsp_stream::launch_session_t::display_helper_gate_status_e::proceed_gaveup) {
          BOOST_LOG(warning) << "Display-helper verification was inconclusive; proceeding with GPU capability probing.";
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Display-helper verification wait failed (" << e.what() << "); proceeding with GPU capability probing.";
      } catch (...) {
        BOOST_LOG(warning) << "Display-helper verification wait failed; proceeding with GPU capability probing.";
      }
    }

    bool has_stream_session_activity();
    bool has_active_or_stopping_stream_session();

    http_encoder_capabilities_t advertised_encoder_capabilities_for_http() {
      std::optional<video::encoder_probe_adapter_hint_lease_t> idle_virtual_adapter_hint;
      std::optional<LUID> idle_virtual_required_adapter;
      if (config::video.virtual_display_mode != config::video_t::virtual_display_mode_e::disabled &&
          !has_stream_session_activity()) {
        const auto intended_adapter = platf::resolve_preferred_render_adapter(
          config::video.adapter_name,
          config::video.adapter_pnp_id
        );
        if (intended_adapter) {
          idle_virtual_required_adapter = *intended_adapter.luid;
          idle_virtual_adapter_hint =
            video::set_pending_virtual_display_adapter_hint(*intended_adapter.luid);
        }
      }
      auto idle_virtual_adapter_hint_guard = util::fail_guard([&] {
        if (idle_virtual_adapter_hint) {
          (void) video::clear_pending_virtual_display_adapter_hint(*idle_virtual_adapter_hint);
        }
      });

      const auto publish = [](
                             video::advertised_encoder_capabilities_t caps,
                             const bool probe_complete,
                             const std::string_view reason
                           ) {
        BOOST_LOG(debug)
          << "HTTP encoder capabilities: probe_complete=" << probe_complete
          << ", hdr="
          << (caps.hevc_mode == 3 || caps.av1_mode == 3)
          << ", hevc_mode=" << caps.hevc_mode
          << ", av1_mode=" << caps.av1_mode
          << ", source=" << reason << '.';
        return http_encoder_capabilities_t {
          .advertised = std::move(caps),
          .probe_complete = probe_complete,
        };
      };

      bool probe_complete = false;
      auto caps = video::advertised_encoder_capabilities(false, &probe_complete);
      if (probe_complete) {
        return publish(std::move(caps), true, "matching-cache");
      }

      // Session starts publish their pending owner while holding this gate.
      // Hold it through the idle decision and temporary-display probe so a
      // new start cannot enter between the counter samples below.
      std::unique_lock<std::mutex> lifecycle_lock(
        stream_lifecycle_mutex(),
        std::try_to_lock
      );
      if (!lifecycle_lock.owns_lock()) {
        BOOST_LOG(debug) << "Skipping HTTP encoder capability probe while stream lifecycle work owns the gate.";
        return publish(std::move(caps), false, "lifecycle-gate");
      }
      caps = video::advertised_encoder_capabilities(false, &probe_complete);
      if (probe_complete) {
        return publish(std::move(caps), true, "matching-cache-after-gate");
      }

      if (has_active_or_stopping_stream_session()) {
        BOOST_LOG(debug) << "Skipping HTTP encoder capability probe while a streaming session is active or stopping.";
        return publish(std::move(caps), false, "active-or-stopping-session");
      }

#ifdef _WIN32
      // Startup probing already waits for the interactive desktop. Keep idle
      // HTTP discovery on the same side of that boundary so a pre-login
      // request cannot create a probe display that Windows cannot enumerate.
      // Stream-initiated probing remains independent of this gate.
      if (!platf::is_default_input_desktop_active()) {
        BOOST_LOG(info) << "HTTP encoder capability probe deferred until the interactive desktop is ready.";
        return publish(std::move(caps), false, "interactive-desktop");
      }
#endif

      auto ensure_result = VDISPLAY::ensure_display(idle_virtual_required_adapter);
      auto cleanup_probe_display = util::fail_guard([&ensure_result]() {
        VDISPLAY::cleanup_ensure_display(ensure_result);
      });
      if (!ensure_result.ready_for_probe()) {
        BOOST_LOG(info)
          << "HTTP encoder capability probe deferred: the exact retained display target is not ready.";
        return publish(std::move(caps), false, "target-pending");
      }
      caps = video::advertised_encoder_capabilities(true, &probe_complete);
      return publish(std::move(caps), probe_complete, "idle-probe");
    }

    void cleanup_virtual_display_state() {
      stream::session::cleanup_reservation_t cleanup_reservation;
      const bool has_active_display = has_active_virtual_display();
      const bool has_retained_probe_display = VDISPLAY::has_retained_ensure_display();
      if (!has_active_display && !has_retained_probe_display) {
        BOOST_LOG(debug) << "Skipping virtual display cleanup after cancel because no active virtual display exists.";
        return;
      }
      if (!has_active_display) {
        BOOST_LOG(info) << "Removing retained encoder-probe virtual display after cancel.";
        VDISPLAY::cleanup_retained_ensure_display();
        return;
      }

      const auto cleanup = platf::virtual_display_cleanup::run("cancel", config::video.dd.config_revert_on_disconnect);
      if (cleanup.helper_revert_dispatched) {
        display_helper_integration::stop_watchdog();
      }
    }

    bool has_stream_session_activity() {
      // RTSP removes STOPPING sessions from session_count() before stream::session::join()
      // returns; pending launches/creations reserve the process-wide runtime layer
      // before either protocol publishes an active session.
      return rtsp_stream::has_pending_launch_or_startup() ||
             rtsp_stream::session_count_no_cleanup() > 0 ||
             stream::session::running_sessions.load(std::memory_order_acquire) != 0 ||
             stream::session::teardown_sessions.load(std::memory_order_acquire) != 0 ||
             webrtc_stream::has_active_or_pending_sessions() ||
             webrtc_stream::has_capture_active() ||
             webrtc_stream::has_teardown_in_progress();
    }

    bool has_active_or_stopping_stream_session() {
      // Sample the generic/VDD cleanup signals on both sides of the protocol
      // activity snapshot. This closes both counter handoffs:
      //   launch: generic reservation -> pending protocol owner
      //   teardown: protocol owner -> generic cleanup reservation
      // Each writer publishes the successor before releasing the predecessor.
      if (platf::virtual_display_cleanup::in_progress() ||
          stream::session::cleanup_reservations.load(std::memory_order_acquire) != 0) {
        return true;
      }
      if (has_stream_session_activity()) {
        return true;
      }
      return platf::virtual_display_cleanup::in_progress() ||
             stream::session::cleanup_reservations.load(std::memory_order_acquire) != 0;
    }

    // Same idleness test as has_active_or_stopping_stream_session() minus the
    // generic cleanup-reservation term. Teardown-path callers hold a reservation
    // across their whole request, so that term reports their own frame as
    // activity and makes the predicate unconditionally true; launch()/resume()
    // teardown guards are reservation-insensitive for the same reason. A
    // concurrent virtual-display cleanup is still honoured, double-sampled
    // around the activity snapshot to close the same handoff.
    bool has_stream_session_activity_or_display_cleanup() {
      if (platf::virtual_display_cleanup::in_progress()) {
        return true;
      }
      if (has_stream_session_activity()) {
        return true;
      }
      return platf::virtual_display_cleanup::in_progress();
    }

    void cleanup_virtual_display_if_idle() {
      try {
        // Serialize the final owner check through cleanup. RTSP launch already
        // holds launch_request_mutex before entering this path; lifecycle is
        // the next canonical gate and also excludes a concurrent WebRTC start.
        std::unique_lock<std::mutex> lifecycle_lock(stream_lifecycle_mutex());
        if (has_stream_session_activity_or_display_cleanup()) {
          BOOST_LOG(info) << "Skipping virtual display cleanup because a streaming session is active or stopping.";
          return;
        }

        cleanup_virtual_display_state();
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Virtual display cleanup failed: " << e.what();
      } catch (...) {
        BOOST_LOG(warning) << "Virtual display cleanup failed with an unknown exception.";
      }
    }

    void prepare_virtual_display_for_session(
      const std::shared_ptr<rtsp_stream::launch_session_t> &launch_session,
      bool no_active_sessions,
      bool allow_display_changes,
      bool is_input_only,
      std::optional<std::string> &pending_output_override,
      std::optional<video::encoder_probe_adapter_hint_lease_t> &pending_adapter_hint,
      const std::function<bool()> &display_startup_cancelled,
      const std::chrono::steady_clock::time_point display_startup_deadline
    ) {

      // This routine is the authoritative resolver for normal NVHTTP launch
      // and resume requests. proc::execute must not later re-enable a virtual
      // display after a per-app physical output override selected one.
      launch_session->virtual_display_request_resolved = true;

      auto disable_virtual_display_request = [&]() {
        launch_session->virtual_display = false;
        launch_session->virtual_display_failed = false;
        launch_session->virtual_display_guid_bytes.fill(0);
        launch_session->virtual_display_device_id.clear();
        launch_session->virtual_display_ready_since.reset();
        launch_session->virtual_display_hdr_enabled.reset();
      };

      std::optional<std::string> app_output_override;
      if (launch_session->output_name_override) {
        app_output_override = boost::algorithm::trim_copy(*launch_session->output_name_override);
      }

      if (app_output_override && !app_output_override->empty() && VDISPLAY::is_virtual_display_selection(*app_output_override)) {
        launch_session->virtual_display = true;
        app_output_override.reset();
      }
      launch_session->virtual_display_recreated_on_demand = false;
      launch_session->virtual_display_needs_resume_apply = false;

      bool config_requests_virtual = config::video.virtual_display_mode != config::video_t::virtual_display_mode_e::disabled;
      if (launch_session->virtual_display_mode_override) {
        config_requests_virtual =
          *launch_session->virtual_display_mode_override != config::video_t::virtual_display_mode_e::disabled;
      }
      const bool forced_sudavda_virtual_display =
        config::video.output_name == VDISPLAY::SUDOVDA_VIRTUAL_DISPLAY_SELECTION;
      const bool client_requests_virtual = launch_session->client_virtual_display_override.value_or(
        launch_session->client_requests_virtual_display
      );
      const bool session_requests_virtual = launch_session->app_metadata && launch_session->app_metadata->virtual_screen;
      const bool launch_requests_physical = launch_session->client_virtual_display_override &&
                                            !*launch_session->client_virtual_display_override;
      bool request_virtual_display =
        launch_session->virtual_display ||
        (config_requests_virtual && !launch_requests_physical) ||
        client_requests_virtual || session_requests_virtual || forced_sudavda_virtual_display;
      const auto requested_virtual_display_mode =
        launch_session->virtual_display_mode_override.value_or(config::video.virtual_display_mode);
      const bool shared_virtual_display_mode =
        requested_virtual_display_mode == config::video_t::virtual_display_mode_e::shared;
      auto shared_virtual_display_uuid = VDISPLAY::persistentVirtualDisplayUuid();
      if (shared_virtual_display_mode && !http::shared_virtual_display_guid.empty()) {
        try {
          shared_virtual_display_uuid =
            uuid_util::uuid_t::parse(http::shared_virtual_display_guid);
        } catch (...) {
          // Creation uses the same persistent fallback and repairs the stored value.
        }
      }
      const std::string virtual_display_stable_id =
        shared_virtual_display_mode ?
          shared_virtual_display_uuid.string() :
          (!launch_session->client_uuid.empty() ?
             launch_session->client_uuid :
             launch_session->unique_id);
      const auto virtual_display_stable_uuid =
        VDISPLAY::virtualDisplayUuidFromStableId(virtual_display_stable_id);
      GUID virtual_display_stable_guid {};
      std::memcpy(
        &virtual_display_stable_guid,
        virtual_display_stable_uuid.b8,
        sizeof(virtual_display_stable_guid)
      );
      bool has_app_output_override = app_output_override.has_value();
      auto make_framegen_policy = [&](bool uses_virtual_display) {
        return framegen::make_stream_start_policy({
          .fps = launch_session->fps,
          .fps_scaled = launch_session->fps,
          .display_refresh_millihz = launch_session->client_display_refresh_millihz,
          .frame_generation_enabled = launch_session->frame_generation_enabled,
          .gen1_framegen_fix = launch_session->gen1_framegen_fix,
          .gen2_framegen_fix = launch_session->gen2_framegen_fix,
          .lossless_scaling_framegen = launch_session->lossless_scaling_framegen,
          .lossless_rtss_limit = launch_session->lossless_scaling_rtss_limit,
          .frame_generation_provider = launch_session->frame_generation_provider,
          .uses_virtual_display = uses_virtual_display,
          .capture_mode = config::video.capture,
          .auto_capture_uses_wgc = platf::dxgi::should_use_wgc_default(),
          .auto_virtual_framegen_limiter = config::frame_limiter.virtual_display_limiter_enabled(),
          .virtual_display_refresh_multiplier = config::frame_limiter.fixed_virtual_display_refresh_multiplier(),
        });
      };
      const auto requested_display_framegen_policy =
        make_framegen_policy(has_app_output_override ? false : request_virtual_display);
      const bool framegen_requires_virtual_display = requested_display_framegen_policy.requires_virtual_display;
      if (framegen_requires_virtual_display) {
        request_virtual_display = true;
        app_output_override.reset();
        has_app_output_override = false;
      }
      auto apply_framegen_refresh_policy = [&](bool uses_virtual_display) {
        const auto framegen_policy = make_framegen_policy(uses_virtual_display);
        launch_session->framegen_refresh_rate = framegen_policy.framegen_refresh_rate;
        launch_session->framegen_refresh_millihz = framegen_policy.framegen_refresh_millihz;
        launch_session->framegen_refresh_multiplier = framegen_policy.refresh_multiplier;
      };
      BOOST_LOG(debug) << "Display helper: session prep client='" << launch_session->client_name
                       << "' allow_display_changes=" << allow_display_changes
                       << " no_active_sessions=" << no_active_sessions
                       << " request_virtual_display=" << request_virtual_display
                       << " framegen_requires_virtual_display=" << framegen_requires_virtual_display
                       << " previous_virtual_device_id='" << launch_session->virtual_display_device_id
                       << "' active_output='" << config::get_active_output_name()
                       << "' app_output_override='" << (app_output_override ? *app_output_override : std::string {})
                       << "'.";

      if (!VDISPLAY::policy::should_prepare_display_for_new_session(no_active_sessions)) {
        const auto previous_virtual_display_device_id = launch_session->virtual_display_device_id;
        launch_session->virtual_display = false;
        launch_session->virtual_display_failed = request_virtual_display;
        launch_session->virtual_display_guid_bytes.fill(0);
        launch_session->virtual_display_device_id.clear();
        launch_session->virtual_display_ready_since.reset();
        launch_session->virtual_display_hdr_enabled.reset();
        launch_session->virtual_display_recreated_on_demand = false;
        launch_session->virtual_display_needs_resume_apply = false;
        if (request_virtual_display) {
          const auto existing_device =
            VDISPLAY::resolveActiveVirtualDisplayDeviceIdForStableId(
              virtual_display_stable_id,
              previous_virtual_display_device_id,
              launch_session->client_name,
              VDISPLAY::policy::allow_generic_resume_fallback()
            );
          if (existing_device &&
              VDISPLAY::configuredRenderAdapterMatchesVirtualDisplay(
                virtual_display_stable_guid,
                "active RTSP/shared virtual display reuse"
              )) {
            launch_session->virtual_display = true;
            launch_session->virtual_display_failed = false;
            launch_session->virtual_display_device_id = *existing_device;
            launch_session->virtual_display_ready_since = std::chrono::steady_clock::now();
            launch_session->virtual_display_hdr_enabled.reset();
            apply_framegen_refresh_policy(true);
            BOOST_LOG(info) << "Display helper: another session is active; joining its validated virtual capture target (device_id="
                            << *existing_device << ").";
          } else if (existing_device) {
            BOOST_LOG(error) << "Existing virtual display does not match the configured capture adapter; refusing to claim shared-session virtual-display reuse.";
          } else {
            BOOST_LOG(warning) << "Another session is active, but no reusable virtual display was found for this request.";
          }
        } else {
          apply_framegen_refresh_policy(false);
          BOOST_LOG(info) << "Display helper: another session is active; joining its existing capture target without display changes.";
        }
        return;
      }

      bool pinned_virtual_display_mode_disabled = false;
      if (has_app_output_override && !framegen_requires_virtual_display) {
        request_virtual_display = false;
        disable_virtual_display_request();
        if (!launch_session->virtual_display_mode_override) {
          launch_session->virtual_display_mode_override = config::video_t::virtual_display_mode_e::disabled;
          pinned_virtual_display_mode_disabled = true;
        }
      }

      if (!allow_display_changes) {
        if (request_virtual_display) {
          if (auto existing_device =
                VDISPLAY::resolveActiveVirtualDisplayDeviceIdForStableId(
                  virtual_display_stable_id,
                  launch_session->virtual_display_device_id,
                  launch_session->client_name,
                  VDISPLAY::policy::allow_generic_resume_fallback()
                )) {
            if (VDISPLAY::configuredRenderAdapterMatchesVirtualDisplay(
                  virtual_display_stable_guid,
                  "RTSP resume virtual display reuse"
                )) {
              launch_session->virtual_display = true;
              launch_session->virtual_display_failed = false;
              launch_session->virtual_display_device_id = *existing_device;
              launch_session->virtual_display_ready_since = std::chrono::steady_clock::now();
              launch_session->virtual_display_hdr_enabled.reset();
              launch_session->virtual_display_needs_resume_apply = true;
              config::set_runtime_output_name_override(*existing_device);
              pending_output_override = *existing_device;
              apply_framegen_refresh_policy(true);
              BOOST_LOG(info) << "Display helper: preserving virtual display capture target for resume (device_id="
                              << *existing_device << ").";
              BOOST_LOG(debug) << "Display helper: preserving capture target and refreshing display state for resume.";
              return;
            }

            launch_session->virtual_display = false;
            launch_session->virtual_display_failed = true;
            launch_session->virtual_display_device_id.clear();
            launch_session->virtual_display_ready_since.reset();
            launch_session->virtual_display_hdr_enabled.reset();
            BOOST_LOG(warning) << "Display helper: existing resume virtual display is on a different or unknown adapter; recreating it on demand.";
          }

          BOOST_LOG(info) << "Display helper: resume requested virtual display capture but no reusable virtual display was found;"
                          << " recreating one on demand.";
          launch_session->virtual_display_recreated_on_demand = true;
        } else {
          if (app_output_override) {
            config::set_runtime_output_name_override(*app_output_override);
            pending_output_override = *app_output_override;
            apply_framegen_refresh_policy(false);
            BOOST_LOG(info) << "Display helper: preserving output override for resume: "
                            << (app_output_override->empty() ? "primary display" : *app_output_override);
          } else {
            BOOST_LOG(debug) << "Display helper: skipping virtual display changes for resume.";
          }
          return;
        }
      }

      // Snapshot current display state BEFORE any display enumeration.
      // queryDisplayConfig(QueryType::All) in output_exists() and other calls can activate
      // external dummy plugs, which would pollute the snapshot used for session restore.
      if (no_active_sessions) {
        if (!display_helper_integration::snapshot_current_display_state(
              display_startup_cancelled,
              display_startup_deadline)) {
          BOOST_LOG(warning) << "Display helper snapshot before session start was not accepted.";
        }
      }


      if (app_output_override) {
        config::set_runtime_output_name_override(*app_output_override);
        pending_output_override = *app_output_override;
        BOOST_LOG(info) << "App-specific display override requested: output_name="
                        << (app_output_override->empty() ? "primary display" : *app_output_override);
      }
      BOOST_LOG(debug) << "config_requests_virtual: " << config_requests_virtual;
      BOOST_LOG(debug) << "client_requests_virtual: " << client_requests_virtual;
      BOOST_LOG(debug) << "session_requests_virtual: " << session_requests_virtual;
      BOOST_LOG(debug) << "framegen_requires_virtual_display: " << framegen_requires_virtual_display;
      BOOST_LOG(debug) << "request_virtual_display: " << request_virtual_display;

      const auto requested_output_name = app_output_override ? *app_output_override : config::get_active_output_name();
      if (has_app_output_override) {
        request_virtual_display = false;
        disable_virtual_display_request();
      }

      if (!request_virtual_display && !requested_output_name.empty()) {
        if (!display_device::output_exists(requested_output_name)) {
          BOOST_LOG(warning) << "Requested display '" << requested_output_name
                             << "' not found; initializing virtual display instead.";
          if (!has_app_output_override) {
            request_virtual_display = true;
          }
        }
      }
      if (is_input_only) {
        disable_virtual_display_request();
      } else {
        auto apply_virtual_display_request = [&](bool should_request_virtual_display) {
          if (!should_request_virtual_display) {
            disable_virtual_display_request();
            return;
          }

          const auto intended_adapter = platf::resolve_preferred_render_adapter(
            config::video.adapter_name,
            config::video.adapter_pnp_id
          );
          if (intended_adapter) {
            pending_adapter_hint =
              video::set_pending_virtual_display_adapter_hint(*intended_adapter.luid);
          } else {
            BOOST_LOG(warning)
              << "Cannot publish the pending virtual-display adapter identity before creation (status="
              << platf::adapter_resolution_status_name(intended_adapter.status) << ").";
          }

          if (proc::vDisplayDriverStatus.load(std::memory_order_acquire) != VDISPLAY::DRIVER_STATUS::OK) {
            proc::initVDisplayDriver();
            const auto driver_status = proc::vDisplayDriverStatus.load(std::memory_order_acquire);
            if (driver_status != VDISPLAY::DRIVER_STATUS::OK) {
              BOOST_LOG(warning) << "SudaVDA driver unavailable (status=" << static_cast<int>(driver_status) << "). Continuing with best-effort virtual display creation.";
            }
          }

          auto parse_uuid = [](const std::string &value) -> std::optional<uuid_util::uuid_t> {
            if (value.empty()) {
              return std::nullopt;
            }
            try {
              return uuid_util::uuid_t::parse(value);
            } catch (...) {
              return std::nullopt;
            }
          };

          auto ensure_shared_guid = [&]() -> uuid_util::uuid_t {
            if (!http::shared_virtual_display_guid.empty()) {
              if (auto parsed = parse_uuid(http::shared_virtual_display_guid)) {
                return *parsed;
              }
            }
            auto generated = VDISPLAY::persistentVirtualDisplayUuid();
            http::shared_virtual_display_guid = generated.string();
            nvhttp::save_state();
            return generated;
          };

          const bool shared_mode = shared_virtual_display_mode;
          uuid_util::uuid_t session_uuid;
          if (shared_mode) {
            session_uuid = ensure_shared_guid();
            launch_session->unique_id = session_uuid.string();
          } else if (auto parsed = parse_uuid(launch_session->unique_id)) {
            session_uuid = *parsed;
          } else {
            session_uuid = VDISPLAY::persistentVirtualDisplayUuid();
            launch_session->unique_id = session_uuid.string();
          }

          std::string display_uuid_source;
          if (!shared_mode && !launch_session->client_uuid.empty()) {
            display_uuid_source = launch_session->client_uuid;
            BOOST_LOG(debug) << "Using client UUID for virtual display: " << display_uuid_source;
          } else {
            display_uuid_source = session_uuid.string();
            BOOST_LOG(debug) << "Using session UUID for virtual display: " << display_uuid_source;
          }

          GUID virtual_display_guid {};
          if (!shared_mode && !launch_session->client_uuid.empty()) {
            const auto client_virtual_display_uuid = VDISPLAY::virtualDisplayUuidFromStableId(launch_session->client_uuid);
            std::memcpy(&virtual_display_guid, client_virtual_display_uuid.b8, sizeof(virtual_display_guid));
            std::copy_n(
              std::cbegin(client_virtual_display_uuid.b8),
              sizeof(client_virtual_display_uuid.b8),
              launch_session->virtual_display_guid_bytes.begin()
            );
          } else {
            std::memcpy(&virtual_display_guid, session_uuid.b8, sizeof(virtual_display_guid));
            std::copy_n(std::cbegin(session_uuid.b8), sizeof(session_uuid.b8), launch_session->virtual_display_guid_bytes.begin());
          }

          uint32_t vd_width = launch_session->resolution_override ?
                                static_cast<uint32_t>(launch_session->resolution_override->width) :
                                (launch_session->width > 0 ? static_cast<uint32_t>(launch_session->width) : 1920u);
          uint32_t vd_height = launch_session->resolution_override ?
                                 static_cast<uint32_t>(launch_session->resolution_override->height) :
                                 (launch_session->height > 0 ? static_cast<uint32_t>(launch_session->height) : 1080u);
          // Virtual-display creation may eagerly enable HDR. Default to no state change so
          // "Do not change HDR" preserves the retained Windows setting.
          bool virtual_display_hdr_requested = false;
          display_helper_integration::helpers::SessionDisplayConfigurationHelper initial_display_helper(config::video, *launch_session, true);
          if (auto initial_configuration = initial_display_helper.initial_virtual_display_configuration()) {
            if (initial_configuration->m_resolution &&
                initial_configuration->m_resolution->m_width > 0 &&
                initial_configuration->m_resolution->m_height > 0) {
              vd_width = initial_configuration->m_resolution->m_width;
              vd_height = initial_configuration->m_resolution->m_height;
              BOOST_LOG(info) << "Virtual display initial resolution resolved from display configuration: "
                              << vd_width << 'x' << vd_height;
            }
            if (initial_configuration->m_hdr_state) {
              virtual_display_hdr_requested =
                *initial_configuration->m_hdr_state == display_device::HdrState::Enabled;
            }
          }
          const uint32_t base_vd_fps_millihz = launch_session->client_display_refresh_millihz > 0 ?
                                                     launch_session->client_display_refresh_millihz :
                                                     framegen::normalize_refresh_millihz(launch_session->fps);
          uint32_t vd_fps = rtsp_stream::effective_display_refresh_millihz(*launch_session);
          if (vd_fps == 0) {
            vd_fps = 60000u;
          }
          const bool framegen_refresh_active =
            (launch_session->framegen_refresh_millihz && *launch_session->framegen_refresh_millihz > 0) ||
            (launch_session->framegen_refresh_rate && *launch_session->framegen_refresh_rate > 0);
          const int refresh_multiplier =
            framegen_refresh_active ? rtsp_stream::framegen_refresh_multiplier(*launch_session) : 1;
          if (base_vd_fps_millihz > 0 && refresh_multiplier > 1) {
            const uint64_t minimum = static_cast<uint64_t>(base_vd_fps_millihz) * static_cast<uint64_t>(refresh_multiplier);
            vd_fps = std::max(vd_fps, static_cast<uint32_t>(std::min<uint64_t>(minimum, std::numeric_limits<uint32_t>::max())));
          }

          std::string client_label;
          if (shared_mode) {
            client_label = config::nvhttp.sunshine_name.empty() ? "Sunshine Shared Display" : config::nvhttp.sunshine_name + " Shared";
          } else {
            if (!launch_session->client_name.empty()) {
              client_label = launch_session->client_name;
            } else if (!launch_session->device_name.empty()) {
              client_label = launch_session->device_name;
            } else {
              client_label = config::nvhttp.sunshine_name;
            }
            if (client_label.empty()) {
              client_label = "Sunshine";
            }
          }

          const auto desired_layout = launch_session->virtual_display_layout_override.value_or(config::video.virtual_display_layout);
          const bool wants_extended_layout = desired_layout != config::video_t::virtual_display_layout_e::exclusive;
          if (wants_extended_layout) {
            // HTTP capability discovery can retain a temporary virtual display. The
            // stream creation path removes that probe before it creates the session
            // display, so never carry any existing virtual identity into the stream
            // baseline. The request helper adds exactly the new session display.
            auto topology_snapshot = display_helper_integration::capture_physical_topology();
            if (topology_snapshot) {
              launch_session->virtual_display_topology_snapshot = *topology_snapshot;
            } else {
              launch_session->virtual_display_topology_snapshot.reset();
            }
          } else {
            launch_session->virtual_display_topology_snapshot.reset();
          }


          // Capture physical monitor refresh rates before VD creation so they can be
          // restored after the virtual display is configured (VD creation at (0,0) can
          // cause Windows to reset other monitors' refresh rates).
          if (auto pre_vd_devices = display_helper_integration::enumerate_devices()) {
            std::map<std::string, std::pair<unsigned int, unsigned int>> rates;
            for (const auto &device : *pre_vd_devices) {
              if (device.m_device_id.empty() || !device.m_info ||
                  VDISPLAY::is_virtual_display_output(device.m_device_id)) {
                continue;
              }
              if (const auto *rat = std::get_if<display_device::Rational>(&device.m_info->m_refresh_rate)) {
                rates[device.m_device_id] = {rat->m_numerator, rat->m_denominator};
              } else if (const auto *dbl = std::get_if<double>(&device.m_info->m_refresh_rate)) {
                auto num = static_cast<unsigned int>(std::round(*dbl * 1000));
                rates[device.m_device_id] = {num, 1000u};

              }
            }
            if (!rates.empty()) {
              launch_session->pre_virtual_display_refresh_rates = std::move(rates);
            }
          }
          VDISPLAY::setWatchdogFeedingEnabled(true);
          const char *hdr_profile = launch_session->hdr_profile ? launch_session->hdr_profile->c_str() : nullptr;
          auto display_info = VDISPLAY::createVirtualDisplay(
            display_uuid_source.c_str(),
            client_label.c_str(),
            hdr_profile,
            vd_width,
            vd_height,
            vd_fps,
            virtual_display_guid,
            base_vd_fps_millihz,
            framegen_refresh_active,
            refresh_multiplier,
            virtual_display_hdr_requested,
            false,
            !shared_mode
          );
          if (display_info) {
            launch_session->virtual_display = true;
            launch_session->virtual_display_failed = false;
            if (display_info->device_id && !display_info->device_id->empty()) {
              launch_session->virtual_display_device_id = *display_info->device_id;
            } else if (auto resolved_device = VDISPLAY::resolveActiveVirtualDisplayDeviceIdForStableId(
                         display_uuid_source,
                         launch_session->virtual_display_device_id,
                         client_label,
                         VDISPLAY::policy::allow_generic_resume_fallback()
                       )) {
              launch_session->virtual_display_device_id = *resolved_device;
            } else {
              launch_session->virtual_display_device_id.clear();
            }
            if (!launch_session->virtual_display_device_id.empty()) {
              config::set_runtime_output_name_override(launch_session->virtual_display_device_id);
              pending_output_override = launch_session->virtual_display_device_id;
              if (pending_adapter_hint) {
                (void) video::mark_pending_virtual_display_adapter_hint_ready_for_verification(
                  *pending_adapter_hint
                );
              }
            }
            launch_session->virtual_display_ready_since = display_info->ready_since;
            launch_session->virtual_display_hdr_enabled = display_info->hdr_enabled;
            if (display_info->display_name && !display_info->display_name->empty()) {
              BOOST_LOG(info) << "Virtual display created at " << platf::to_utf8(*display_info->display_name);
            } else {
              BOOST_LOG(info) << "Virtual display created (device name pending enumeration).";
            }

            VDISPLAY::VirtualDisplayRecoveryParams recovery_params;
            recovery_params.guid = virtual_display_guid;
            recovery_params.width = vd_width;
            recovery_params.height = vd_height;
            recovery_params.fps = vd_fps;
            recovery_params.base_fps_millihz = base_vd_fps_millihz;
            recovery_params.framegen_refresh_active = framegen_refresh_active;
            recovery_params.framegen_refresh_multiplier = refresh_multiplier;
            recovery_params.hdr_requested = virtual_display_hdr_requested;
            recovery_params.client_uid = display_uuid_source;
            recovery_params.client_name = client_label;
            recovery_params.hdr_profile = launch_session->hdr_profile;
            recovery_params.display_name = display_info->display_name;
            recovery_params.monitor_device_path = display_info->monitor_device_path;
            if (display_info->device_id && !display_info->device_id->empty()) {
              recovery_params.device_id = *display_info->device_id;
            } else if (!launch_session->virtual_display_device_id.empty()) {
              recovery_params.device_id = launch_session->virtual_display_device_id;
            }
            recovery_params.max_attempts = 3;

            GUID recovery_guid = virtual_display_guid;
            recovery_params.should_abort = [recovery_guid]() {
              return !VDISPLAY::is_virtual_display_guid_tracked(recovery_guid);
            };
            auto recovery_session = std::make_shared<rtsp_stream::launch_session_t>(
              display_helper_integration::helpers::make_display_request_session_snapshot(*launch_session)
            );
            recovery_params.confirmed_active_at_schedule = display_info->confirmed_active;
            recovery_params.on_recovery_success = [recovery_session](const VDISPLAY::VirtualDisplayCreationResult &result, std::stop_token stop_token) -> std::function<void()> {
              const auto cancelled = [&] {
                return stop_token.stop_requested();
              };
              std::optional<config::runtime_output_override_lease_t> recovery_output_override_lease;
              auto clear_recovery_output_override = util::fail_guard([&] {
                if (recovery_output_override_lease) {
                  (void) config::clear_runtime_output_name_override_if_lease(*recovery_output_override_lease);
                }
              });
              const auto wait_or_cancel = [&](std::chrono::milliseconds delay) {
                const auto deadline = std::chrono::steady_clock::now() + delay;
                while (!cancelled()) {
                  const auto now = std::chrono::steady_clock::now();
                  if (now >= deadline) {
                    return false;
                  }
                  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
                  std::this_thread::sleep_for(std::min(std::max(remaining, std::chrono::milliseconds(1)), std::chrono::milliseconds(50)));
                }
                return true;
              };

              if (cancelled()) {
                return {};
              }
              if (result.device_id && !result.device_id->empty()) {
                recovery_session->virtual_display_device_id = *result.device_id;
                if (cancelled()) {
                  return {};
                }
                recovery_output_override_lease = config::set_runtime_output_name_override_with_lease(
                  recovery_session->virtual_display_device_id
                );
              }
              if (cancelled()) {
                return {};
              }
              recovery_session->virtual_display_ready_since = result.ready_since;
              recovery_session->virtual_display_hdr_enabled = result.hdr_enabled;
              if (recovery_session->virtual_display) {
                // Re-apply display configuration synchronously on the recovery monitor thread.
                // Running this inline (blocking) prevents the recovery monitor from polling during
                // topology churn caused by APPLY, which would otherwise cause transient CCD
                // enumeration failures that look like the display disappeared.

                constexpr int kMaxApplyAttempts = 5;
                bool applied = false;

                for (int attempt = 1; attempt <= kMaxApplyAttempts; ++attempt) {
                  if (cancelled()) {
                    return {};
                  }
                  (void) display_helper_integration::disarm_pending_restore(cancelled);
                  if (cancelled()) {
                    return {};
                  }

                  auto request = display_helper_integration::helpers::build_request_from_session(config::video, *recovery_session);
                  if (!request) {
                    BOOST_LOG(warning) << "Virtual display recovery: failed to rebuild display helper request after recreation (attempt "
                                       << attempt << "/" << kMaxApplyAttempts << ").";
                    if (wait_or_cancel(std::chrono::milliseconds(250 + (attempt - 1) * 250))) {
                      return {};
                    }
                    continue;
                  }

                  if (cancelled()) {
                    return {};
                  }
                  // This recovery worker is torn down with the session, so it
                  // keeps the short shutdown-class helper IPC timeouts.
                  if (display_helper_integration::apply(
                        *request,
                        nullptr,
                        cancelled,
                        display_helper_integration::ApplyRetryPolicy::Full,
                        {},
                        true)) {
                    BOOST_LOG(info) << "Virtual display recovery: re-applied session display configuration (including exclusivity) after recreation.";
                    applied = true;
                    break;
                  }
                  if (cancelled()) {
                    return {};
                  }

                  BOOST_LOG(warning) << "Virtual display recovery: display helper apply failed after recreation (attempt "
                                     << attempt << "/" << kMaxApplyAttempts << ").";
                  if (wait_or_cancel(std::chrono::milliseconds(250 + (attempt - 1) * 250))) {
                    return {};
                  }
                }

                if (!cancelled() && mail::man) {
                  mail::man->event<int>(mail::switch_display)->raise(-1);
                }
                if (cancelled()) {
                  return {};
                }
                BOOST_LOG(info) << "Virtual display recovery: requested capture reinit to pick up recreated display"
                                << (applied ? "." : " (apply did not succeed).");
              }
              std::function<void()> rollback_output_override;
              if (recovery_output_override_lease) {
                const auto lease = *recovery_output_override_lease;
                rollback_output_override = [lease] {
                  (void) config::clear_runtime_output_name_override_if_lease(lease);
                };
              }
              clear_recovery_output_override.disable();
              return rollback_output_override;
            };

            VDISPLAY::schedule_virtual_display_recovery_monitor(recovery_params);
          } else {
            disable_virtual_display_request();
            launch_session->virtual_display = false;
            launch_session->virtual_display_failed = true;
            launch_session->virtual_display_guid_bytes.fill(0);
            launch_session->virtual_display_device_id.clear();
            launch_session->virtual_display_ready_since.reset();
            launch_session->virtual_display_hdr_enabled.reset();
            launch_session->framegen_refresh_rate.reset();
            launch_session->framegen_refresh_millihz.reset();
            launch_session->framegen_refresh_multiplier = 1;
            BOOST_LOG(warning) << "Virtual display creation failed.";
          }
        };

        // A per-app physical output override must never suppress this fallback:
        // should_auto_enable_virtual_display() only returns true when no capturable
        // physical display is active at all, so the pinned output cannot exist and the
        // session would otherwise land on the generic temporary probe display.
        if (!request_virtual_display && VDISPLAY::should_auto_enable_virtual_display()) {
          BOOST_LOG(info) << "No physical monitors detected. Automatically enabling virtual display.";
          request_virtual_display = true;
          if (has_app_output_override) {
            BOOST_LOG(info) << "Dropping unsatisfiable per-app display override '"
                            << (app_output_override && !app_output_override->empty() ? *app_output_override : std::string {"primary display"})
                            << "' because no physical display is available.";
            // Release the pin so capture targets the virtual display created below
            // instead of an output that does not exist.
            config::set_runtime_output_name_override(std::nullopt);
            pending_output_override.reset();
            app_output_override.reset();
            has_app_output_override = false;
            launch_session->output_name_override.reset();
            if (pinned_virtual_display_mode_disabled) {
              // Restore the mode this session would have had without the override so
              // shared/per-client identity stays consistent with virtual_display_stable_id.
              launch_session->virtual_display_mode_override.reset();
              pinned_virtual_display_mode_disabled = false;
            }
          }
        }
        if (allow_display_changes) {
          apply_framegen_refresh_policy(request_virtual_display);

          if (request_virtual_display) {
            // A new virtual-display session supersedes the prior session's restore.
            // Disarm it before any driver mutation; checking first used to return
            // early and made the DISARM below unreachable in the exact race it was
            // intended to prevent.
            const bool virtual_display_mutation_allowed =
              display_helper_integration::request_policy::supersede_restore_for_virtual_display(
                [&] {
                  (void) display_helper_integration::disarm_pending_restore(
                    display_startup_cancelled,
                    display_startup_deadline
                  );
                },
                [&] {
                  return display_helper_integration::restore_in_progress(
                    display_startup_cancelled,
                    display_startup_deadline
                  );
                }
              );
            if (!virtual_display_mutation_allowed) {
              BOOST_LOG(warning) << "Display helper: virtual display creation deferred because physical display restoration is still in progress; using physical fallback for this session.";
              launch_session->virtual_display = false;
              launch_session->virtual_display_failed = true;
              launch_session->virtual_display_guid_bytes.fill(0);
              launch_session->virtual_display_device_id.clear();
              launch_session->virtual_display_ready_since.reset();
              launch_session->virtual_display_hdr_enabled.reset();
              apply_framegen_refresh_policy(false);
              return;
            }
          }

          apply_virtual_display_request(request_virtual_display);
          if (launch_session->virtual_display && !launch_session->virtual_display_device_id.empty()) {
            config::set_runtime_output_name_override(launch_session->virtual_display_device_id);
            pending_output_override = launch_session->virtual_display_device_id;
          }
        } else {
          BOOST_LOG(debug) << "Display helper: skipping virtual display changes for resume.";
        }
      }
    }
    }  // namespace
#endif

#ifndef _WIN32
  namespace {
    bool has_stream_session_activity_for_http_probe() {
      return rtsp_stream::has_pending_launch_or_startup() ||
             rtsp_stream::session_count_no_cleanup() > 0 ||
             stream::session::running_sessions.load(std::memory_order_acquire) != 0 ||
             stream::session::teardown_sessions.load(std::memory_order_acquire) != 0 ||
             webrtc_stream::has_active_or_pending_sessions() ||
             webrtc_stream::has_capture_active() ||
             webrtc_stream::has_teardown_in_progress();
    }

    http_encoder_capabilities_t advertised_encoder_capabilities_for_http() {
      const auto publish = [](video::advertised_encoder_capabilities_t caps, const std::string_view reason) {
        const bool probe_complete = video::has_successful_encoder_probe();
        BOOST_LOG(debug)
          << "HTTP encoder capabilities: probe_complete=" << probe_complete
          << ", hdr="
          << (caps.hevc_mode == 3 || caps.av1_mode == 3)
          << ", hevc_mode=" << caps.hevc_mode
          << ", av1_mode=" << caps.av1_mode
          << ", source=" << reason << '.';
        return http_encoder_capabilities_t {
          .advertised = std::move(caps),
          .probe_complete = probe_complete,
        };
      };

      if (video::has_successful_encoder_probe()) {
        return publish(video::advertised_encoder_capabilities(false), "matching-cache");
      }

      std::unique_lock<std::mutex> lifecycle_lock(stream_lifecycle_mutex(), std::try_to_lock);
      if (!lifecycle_lock.owns_lock()) {
        BOOST_LOG(debug) << "Skipping HTTP encoder capability probe while stream lifecycle work owns the gate.";
        return publish(video::advertised_encoder_capabilities(false), "lifecycle-gate");
      }
      if (video::has_successful_encoder_probe()) {
        return publish(video::advertised_encoder_capabilities(false), "matching-cache-after-gate");
      }
      if (has_stream_session_activity_for_http_probe()) {
        BOOST_LOG(debug) << "Skipping HTTP encoder capability probe while a streaming session is active or stopping.";
        return publish(video::advertised_encoder_capabilities(false), "active-or-stopping-session");
      }

      return publish(video::advertised_encoder_capabilities(true), "idle-probe");
    }
  }  // namespace
#endif

  web_stream_capabilities_t get_web_stream_capabilities() {
    const auto snapshot = advertised_encoder_capabilities_for_http();
    const auto &caps = snapshot.advertised;
    const bool probe_complete = snapshot.probe_complete;
    return {
      .probe_complete = probe_complete,
      .h264 = probe_complete,
      .hevc = probe_complete && caps.hevc_mode >= 2,
      .av1 = probe_complete && caps.av1_mode >= 2,
      .hevc_hdr = probe_complete && caps.hevc_mode >= 3,
      .av1_hdr = probe_complete && caps.av1_mode >= 3,
    };
  }

    // uniqueID, session
    std::unordered_map<std::string, pair_session_t> map_id_sess;
    client_t client_root;
    std::mutex client_mutex;
    std::atomic<uint32_t> session_id_counter;


    using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
    using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;
    using resp_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>;
    using req_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request>;

    enum class op_e {
      ADD,  ///< Add certificate
      REMOVE  ///< Remove certificate
    };

    client_t client_root_snapshot() {
      std::lock_guard<std::mutex> lock(client_mutex);
      client_t snapshot;
      snapshot.named_devices.reserve(client_root.named_devices.size());
      for (const auto &named_cert : client_root.named_devices) {
        if (named_cert) {
          snapshot.named_devices.emplace_back(std::make_shared<crypto::named_cert_t>(*named_cert));
        }
      }
      return snapshot;
    }

    std::string get_arg(const args_t &args, const char *name, const char *default_value) {
      auto it = args.find(name);
      if (it == std::end(args)) {
        if (default_value != nullptr) {
          return std::string(default_value);
        }


        throw std::out_of_range(name);
      }
      return it->second;
    }


    // Helper function to extract command entries from a JSON object.
    cmd_list_t extract_command_entries(const nlohmann::json &j, const std::string &key) {
      cmd_list_t commands;


      if (j.contains(key)) {
        try {
          for (const auto &item : j.at(key)) {
            try {
              std::string cmd = item.at("cmd").get<std::string>();
              bool elevated = util::get_non_string_json_value<bool>(item, "elevated", false);
              commands.push_back({cmd, elevated});
            } catch (const std::exception &e) {
              BOOST_LOG(warning) << "Error parsing command entry: " << e.what();
            }
          }
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "Error retrieving key \"" << key << "\": " << e.what();
        }
      } else {
        BOOST_LOG(debug) << "Key \"" << key << "\" not found in the JSON.";

      }

      return commands;
    }


    std::optional<config::video_t::virtual_display_mode_e> parse_virtual_display_mode_override(const std::string &value) {
      if (value.empty()) {
        return std::nullopt;
      }
      const auto normalized = boost::algorithm::to_lower_copy(value);
      if (normalized == "disabled") {
        return config::video_t::virtual_display_mode_e::disabled;
      }
      if (normalized == "per_client") {
        return config::video_t::virtual_display_mode_e::per_client;
      }
      if (normalized == "shared") {
        return config::video_t::virtual_display_mode_e::shared;
      }
      return std::nullopt;
    }


    std::optional<config::video_t::virtual_display_layout_e> parse_virtual_display_layout_override(const std::string &value) {
      if (value.empty()) {
        return std::nullopt;
      }
      const auto normalized = boost::algorithm::to_lower_copy(value);
      if (normalized == "exclusive") {
        return config::video_t::virtual_display_layout_e::exclusive;
      }
      if (normalized == "extended") {
        return config::video_t::virtual_display_layout_e::extended;
      }
      if (normalized == "extended_primary") {
        return config::video_t::virtual_display_layout_e::extended_primary;
      }
      if (normalized == "extended_isolated") {
        return config::video_t::virtual_display_layout_e::extended_isolated;
      }
      if (normalized == "extended_primary_isolated") {
        return config::video_t::virtual_display_layout_e::extended_primary_isolated;
      }
      return std::nullopt;
    }

    void save_state() {
      statefile::migrate_recent_state_keys();
      const auto &sunshine_path = statefile::sunshine_state_path();
      const auto &vibeshine_path = statefile::vibeshine_state_path();
      const bool share_state_file = statefile::share_state_file();
      const client_t client = client_root_snapshot();

      std::lock_guard<std::mutex> state_lock(statefile::state_mutex());

      nlohmann::json root = nlohmann::json::object();
      if (fs::exists(sunshine_path)) {
        try {
          std::ifstream in(sunshine_path);
          in >> root;
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "Couldn't read "sv << sunshine_path << ": "sv << e.what();
          return;
        }
      }

      root["root"] = nlohmann::json::object();
      root["root"]["uniqueid"] = http::unique_id;
      if (share_state_file) {
        root["root"]["last_notified_version"] = update::state.last_notified_version;
      }

      nlohmann::json named_cert_nodes = nlohmann::json::array();

      std::unordered_set<std::string> unique_certs;
      std::unordered_map<std::string, int> name_counts;

      for (auto &named_cert_p : client.named_devices) {
        if (unique_certs.insert(named_cert_p->cert).second) {
          nlohmann::json named_cert_node = nlohmann::json::object();
          std::string base_name = named_cert_p->name;
          size_t pos = base_name.find(" (");
          if (pos != std::string::npos) {
            base_name = base_name.substr(0, pos);
          }
          int count = name_counts[base_name]++;
          std::string final_name = base_name;
          if (count > 0) {
            final_name += " (" + std::to_string(count + 1) + ")";
          }
          named_cert_node["name"] = final_name;
          named_cert_node["cert"] = named_cert_p->cert;
          named_cert_node["uuid"] = named_cert_p->uuid;
          named_cert_node["display_mode"] = named_cert_p->display_mode;
          if (!named_cert_p->virtual_display_mode_override.empty()) {
            named_cert_node["virtual_display_mode"] = named_cert_p->virtual_display_mode_override;
          }
          if (!named_cert_p->virtual_display_layout_override.empty()) {
            named_cert_node["virtual_display_layout"] = named_cert_p->virtual_display_layout_override;
          }
          if (!named_cert_p->hdr_profile.empty()) {
            named_cert_node["hdr_profile"] = named_cert_p->hdr_profile;
          }
          if (!named_cert_p->output_name_override.empty()) {
            named_cert_node["output_name_override"] = named_cert_p->output_name_override;
          }
          named_cert_node["perm"] = static_cast<uint32_t>(named_cert_p->perm);
          named_cert_node["enable_legacy_ordering"] = named_cert_p->enable_legacy_ordering;
          named_cert_node["allow_client_commands"] = named_cert_p->allow_client_commands;
          named_cert_node["always_use_virtual_display"] = named_cert_p->always_use_virtual_display;
          if (named_cert_p->prefer_10bit_sdr) {
            named_cert_node["prefer_10bit_sdr"] = true;
          }
          if (named_cert_p->last_seen.has_value()) {
            named_cert_node["last_seen"] = *named_cert_p->last_seen;
          }
          if (!named_cert_p->config_overrides.empty()) {
            named_cert_node["config_overrides"] = named_cert_p->config_overrides;
          }

          if (!named_cert_p->do_cmds.empty()) {
            nlohmann::json do_cmds_node = nlohmann::json::array();
            for (const auto &cmd : named_cert_p->do_cmds) {
              do_cmds_node.push_back(crypto::command_entry_t::serialize(cmd));
            }
            named_cert_node["do"] = do_cmds_node;
          }

          if (!named_cert_p->undo_cmds.empty()) {
            nlohmann::json undo_cmds_node = nlohmann::json::array();
            for (const auto &cmd : named_cert_p->undo_cmds) {
              undo_cmds_node.push_back(crypto::command_entry_t::serialize(cmd));
            }
            named_cert_node["undo"] = undo_cmds_node;
          }

          named_cert_nodes.push_back(named_cert_node);
        }
      }

      root["root"]["named_devices"] = named_cert_nodes;

      if (file_handler::write_file(sunshine_path.c_str(), root.dump(4)) != 0) {
        BOOST_LOG(error) << "Couldn't write "sv << sunshine_path;
        return;
      }

      if (!share_state_file) {
        auto ensure_root = [](pt::ptree &tree) -> pt::ptree & {
          auto it = tree.find("root");
          if (it == tree.not_found()) {
            auto inserted = tree.insert(tree.end(), std::make_pair(std::string("root"), pt::ptree {}));
            return inserted->second;
          }
          return it->second;
        };

        pt::ptree vibeshine_tree;
        if (fs::exists(vibeshine_path)) {
          try {
            pt::read_json(vibeshine_path, vibeshine_tree);
          } catch (const std::exception &e) {
            BOOST_LOG(error) << "Couldn't read "sv << vibeshine_path << ": "sv << e.what();
            return;
          }
        }

        auto &vibe_root = ensure_root(vibeshine_tree);
        vibe_root.put("last_notified_version", update::state.last_notified_version);

#ifdef _WIN32
        if (!http::shared_virtual_display_guid.empty()) {
          vibe_root.put("shared_virtual_display_guid", http::shared_virtual_display_guid);
        }
#endif

        try {
          statefile::write_json_atomic(vibeshine_path, vibeshine_tree);
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "Couldn't write "sv << vibeshine_path << ": "sv << e.what();
        }
      }
    }

    void load_state() {
      statefile::migrate_recent_state_keys();
      const auto &sunshine_path = statefile::sunshine_state_path();
      const auto &vibeshine_path = statefile::vibeshine_state_path();
      const bool share_state_file = statefile::share_state_file();

      std::lock_guard<std::mutex> state_lock(statefile::state_mutex());

      if (!fs::exists(sunshine_path)) {
        BOOST_LOG(info) << "File "sv << sunshine_path << " doesn't exist"sv;
        http::unique_id = uuid_util::uuid_t::generate().string();
        update::state.last_notified_version.clear();
        return;
      }

      nlohmann::json tree;
      try {
        std::ifstream in(sunshine_path);
        in >> tree;
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Couldn't read "sv << sunshine_path << ": "sv << e.what();
        return;
      }

      nlohmann::json root = tree.contains("root") ? tree["root"] : nlohmann::json::object();


      if (share_state_file) {
        update::state.last_notified_version = root.value("last_notified_version", "");
      } else if (fs::exists(vibeshine_path)) {

        try {
          pt::ptree vibeshine_tree;
          pt::read_json(vibeshine_path, vibeshine_tree);
          update::state.last_notified_version = vibeshine_tree.get("root.last_notified_version", "");
#ifdef _WIN32
          http::shared_virtual_display_guid = vibeshine_tree.get("root.shared_virtual_display_guid", "");
#endif

        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "Couldn't read "sv << vibeshine_path << " for notification state: "sv << e.what();
          update::state.last_notified_version.clear();

#ifdef _WIN32
          http::shared_virtual_display_guid.clear();
#endif
        }
      } else {
        update::state.last_notified_version.clear();
#ifdef _WIN32
        http::shared_virtual_display_guid.clear();
#endif
      }

#ifdef _WIN32
      if (share_state_file && !root.contains("shared_virtual_display_guid")) {
        http::shared_virtual_display_guid.clear();
      }
#endif

      if (!root.contains("uniqueid")) {
        http::uuid = uuid_util::uuid_t::generate();
        http::unique_id = http::uuid.string();
        return;
      }

      std::string uid = root["uniqueid"];
      http::uuid = uuid_util::uuid_t::parse(uid);
      http::unique_id = uid;

      client_t client;

      if (root.contains("devices")) {
        for (auto &device_node : root["devices"]) {
          if (device_node.contains("certs")) {
            for (auto &el : device_node["certs"]) {
              auto named_cert_p = std::make_shared<crypto::named_cert_t>();
              named_cert_p->name = "";
              named_cert_p->cert = el.get<std::string>();
              named_cert_p->uuid = uuid_util::uuid_t::generate().string();
              named_cert_p->display_mode = "";
              named_cert_p->output_name_override.clear();
              named_cert_p->perm = PERM::_all;
              named_cert_p->enable_legacy_ordering = true;
              named_cert_p->allow_client_commands = true;
              named_cert_p->always_use_virtual_display = false;
              named_cert_p->prefer_10bit_sdr = false;
              client.named_devices.emplace_back(named_cert_p);
            }
          }
        }
      }


      if (root.contains("named_devices")) {
        for (auto &el : root["named_devices"]) {
          auto named_cert_p = std::make_shared<crypto::named_cert_t>();
          named_cert_p->name = el.value("name", "");
          named_cert_p->cert = el.value("cert", "");
          named_cert_p->uuid = el.value("uuid", "");
          named_cert_p->display_mode = el.value("display_mode", "");
          named_cert_p->hdr_profile = el.value("hdr_profile", "");
          named_cert_p->output_name_override = el.value("output_name_override", "");
          named_cert_p->virtual_display_mode_override = el.value("virtual_display_mode", "");
          named_cert_p->virtual_display_layout_override = el.value("virtual_display_layout", "");
          named_cert_p->perm = (PERM) (util::get_non_string_json_value<uint32_t>(el, "perm", (uint32_t) PERM::_all)) & PERM::_all;
          named_cert_p->enable_legacy_ordering = util::get_non_string_json_value<bool>(el, "enable_legacy_ordering", true);
          named_cert_p->allow_client_commands = util::get_non_string_json_value<bool>(el, "allow_client_commands", true);
          named_cert_p->always_use_virtual_display = util::get_non_string_json_value<bool>(el, "always_use_virtual_display", false);
          named_cert_p->prefer_10bit_sdr =
            el.contains("prefer_10bit_sdr") && !el["prefer_10bit_sdr"].is_null() &&
            util::get_non_string_json_value<bool>(el, "prefer_10bit_sdr", false);
          if (el.contains("last_seen") && !el["last_seen"].is_null()) {
            named_cert_p->last_seen = util::get_non_string_json_value<std::int64_t>(el, "last_seen", 0);
          } else {
            named_cert_p->last_seen.reset();
          }
          named_cert_p->config_overrides.clear();
          if (el.contains("config_overrides") && el["config_overrides"].is_object()) {
            for (const auto &entry : el["config_overrides"].items()) {
              if (entry.key().empty()) {
                continue;
              }
              named_cert_p->config_overrides[entry.key()] = entry.value().get<std::string>();
            }
          }
          {
            std::unordered_map<std::string, std::string> normalized_overrides;
            config::merge_config_overrides(normalized_overrides, named_cert_p->config_overrides);
            named_cert_p->config_overrides = std::move(normalized_overrides);
          }
          named_cert_p->do_cmds = extract_command_entries(el, "do");
          named_cert_p->undo_cmds = extract_command_entries(el, "undo");
          client.named_devices.emplace_back(named_cert_p);
        }
      }


      {
        std::lock_guard<std::mutex> lock(client_mutex);
        cert_chain.clear();
        for (auto &named_cert : client.named_devices) {
          cert_chain.add(named_cert);

        }

        client_root = client;
      }
    }

    void add_authorized_client(const p_named_cert_t &named_cert_p) {
      {
        std::lock_guard<std::mutex> lock(client_mutex);
        client_root.named_devices.push_back(named_cert_p);
      }

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_paired(named_cert_p->name);
#endif


      if (!config::sunshine.flags[config::flag::FRESH_STATE]) {
        save_state();
        load_state();
      }
    }


    struct resolved_client_identity_t {
      std::string uuid;
      std::string name;
    };

    std::mutex tls_client_identity_mutex;
    std::unordered_map<std::string, resolved_client_identity_t> tls_client_identity_by_endpoint;

    std::string endpoint_key(req_https_t request) {
      if (!request) {
        return {};
      }

      const auto endpoint = request->remote_endpoint();
      if (endpoint.address().is_unspecified() || endpoint.port() == 0) {
        return {};
      }

      return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
    }

    std::optional<resolved_client_identity_t> resolve_client_identity_from_peer_cert(const crypto::x509_t &client_cert) {
      if (!client_cert) {
        BOOST_LOG(debug) << "No client certificate available";
        return std::nullopt;
      }

      const auto client_cert_signature = crypto::signature(client_cert.get());

      std::lock_guard<std::mutex> lock(client_mutex);
      for (const auto &named_cert : client_root.named_devices) {
        if (!named_cert) {
          continue;
        }

        auto stored_x509 = crypto::x509(named_cert->cert);
        if (!stored_x509) {
          continue;
        }

        const auto stored_signature = crypto::signature(stored_x509.get());
        if (stored_signature == client_cert_signature) {
          BOOST_LOG(debug) << "Found matching client UUID: " << named_cert->uuid << " for client: " << named_cert->name;
          return resolved_client_identity_t {
            named_cert->uuid,
            named_cert->name,
          };
        }
      }

      BOOST_LOG(debug) << "No matching client UUID found for certificate";
      return std::nullopt;
    }

    void remember_tls_client_identity(req_https_t request, const resolved_client_identity_t &identity) {
      const auto key = endpoint_key(request);
      if (key.empty() || identity.uuid.empty()) {
        return;
      }

      std::lock_guard<std::mutex> lock(tls_client_identity_mutex);
      tls_client_identity_by_endpoint[key] = identity;
    }

    void forget_tls_client_identity(req_https_t request) {
      const auto key = endpoint_key(request);
      if (key.empty()) {
        return;
      }

      std::lock_guard<std::mutex> lock(tls_client_identity_mutex);
      tls_client_identity_by_endpoint.erase(key);
    }

    std::optional<resolved_client_identity_t> get_remembered_tls_client_identity(req_https_t request) {
      const auto key = endpoint_key(request);
      if (key.empty()) {
        return std::nullopt;
      }

      std::lock_guard<std::mutex> lock(tls_client_identity_mutex);
      const auto it = tls_client_identity_by_endpoint.find(key);
      if (it == tls_client_identity_by_endpoint.end()) {
        return std::nullopt;
      }

      return it->second;
    }

    std::mutex launch_request_mutex;
    std::mutex stream_lifecycle_gate;

    std::mutex &stream_lifecycle_mutex() {
      return stream_lifecycle_gate;
    }

    std::string resolve_known_client_uuid_from_launch_id(const std::string &launch_unique_id) {
      if (launch_unique_id.empty()) {
        return {};
      }

      std::lock_guard<std::mutex> lock(client_mutex);
      for (const auto &named_cert : client_root.named_devices) {
        if (named_cert && named_cert->uuid == launch_unique_id) {
          return launch_unique_id;
        }
      }

      BOOST_LOG(debug) << "Ignoring unmatched launch uniqueid for per-client settings: " << launch_unique_id;
      return {};
    }

    bool is_placeholder_client_name(const std::string &name) {
      const auto trimmed = boost::algorithm::trim_copy(name);
      return boost::iequals(trimmed, "self");
    }

    std::string display_client_name_for_session(const std::string &paired_name, const std::string &device_name, const std::string &host_name) {
      const auto paired = boost::algorithm::trim_copy(paired_name);
      const auto device = boost::algorithm::trim_copy(device_name);
      const auto host = boost::algorithm::trim_copy(host_name);

      if (!paired.empty() && !is_placeholder_client_name(paired)) {
        return paired;
      }
      if (!device.empty() && !is_placeholder_client_name(device)) {
        return device;
      }
      if (!host.empty() && !is_placeholder_client_name(host)) {
        return host;
      }
      return "Sunshine"s;
    }

    std::string client_name_for_uuid(const std::string &uuid) {
      if (uuid.empty()) {
        return {};
      }

      std::lock_guard<std::mutex> lock(client_mutex);
      for (const auto &named_cert : client_root.named_devices) {
        if (named_cert && named_cert->uuid == uuid) {
          return named_cert->name;
        }
      }
      return {};
    }

    resolved_client_identity_t resolve_client_identity(req_https_t request, const verified_client_t &verified_client) {
      if (auto remembered = get_remembered_tls_client_identity(request)) {
        return *remembered;
      }

      resolved_client_identity_t identity;
      if (verified_client) {
        identity.uuid = verified_client->uuid;
        identity.name = verified_client->name;
      }
      return identity;
    }

    static std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session_from_snapshot(
      bool host_audio,
      bool input_only,
      const args_t &args,
      const verified_client_t &verified_client,
      const resolved_client_identity_t *resolved_client_identity
    ) {
      auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();

      launch_session->id = ++session_id_counter;
      launch_session->appid = 0;
      launch_session->gen1_framegen_fix = false;
      launch_session->gen2_framegen_fix = false;
      launch_session->frame_generation_enabled = false;
      launch_session->lossless_scaling_framegen = false;
      launch_session->framegen_refresh_rate.reset();
      launch_session->framegen_refresh_millihz.reset();
      launch_session->framegen_refresh_multiplier = 1;
      launch_session->lossless_scaling_target_fps.reset();
      launch_session->lossless_scaling_rtss_limit.reset();
      launch_session->frame_generation_provider = "lossless-scaling";
      launch_session->client_vrr_requested = false;
#ifdef _WIN32
#endif
      const auto identity_uuid = resolved_client_identity ? resolved_client_identity->uuid : (verified_client ? verified_client->uuid : std::string());
      const auto identity_name = resolved_client_identity ? resolved_client_identity->name : (verified_client ? verified_client->name : std::string());
      launch_session->device_name = identity_name.empty() ? config::nvhttp.sunshine_name : identity_name;
      launch_session->virtual_display = false;
      launch_session->virtual_display_guid_bytes.fill(0);
      launch_session->virtual_display_device_id.clear();
      launch_session->virtual_display_ready_since.reset();
      launch_session->virtual_display_hdr_enabled.reset();
      launch_session->app_metadata.reset();
      launch_session->client_uuid = identity_uuid;
      launch_session->client_name = identity_name;
      launch_session->hdr_profile.reset();
      launch_session->client_display_mode_override = false;
      launch_session->client_display_refresh_millihz = 0;
      launch_session->client_requests_virtual_display = false;
      launch_session->client_virtual_display_override.reset();
      launch_session->resolution_override.reset();
      launch_session->scale_factor = 100;
      launch_session->virtual_display_failed = false;


      auto client_name_arg = get_arg(args, "clientName", "");
      if (!client_name_arg.empty()) {
        if (launch_session->client_name.empty()) {
          launch_session->client_name = client_name_arg;

        }
        launch_session->device_name = client_name_arg;
      }

      // Some launch paths may not provide a peer certificate. Fall back only
      // when the client-provided unique ID is one of our known paired-client
      // UUIDs. Treating placeholder IDs as per-client UUIDs creates a fresh
      // Windows monitor identity and loses DPI/HDR calibration.
      const auto launch_client_uuid = resolve_known_client_uuid_from_launch_id(get_arg(args, "uniqueid", ""));
      if (launch_session->client_uuid.empty()) {
        launch_session->client_uuid = launch_client_uuid;
        launch_session->client_name = client_name_for_uuid(launch_session->client_uuid);
      } else if (!launch_client_uuid.empty() && launch_client_uuid != launch_session->client_uuid && is_placeholder_client_name(launch_session->client_name)) {
        BOOST_LOG(warning) << "Resolved TLS client identity '" << launch_session->client_name
                           << "' is a placeholder and conflicts with launch uniqueid; using paired client UUID "
                           << launch_client_uuid << " for this session.";
        launch_session->client_uuid = launch_client_uuid;
        launch_session->client_name = client_name_for_uuid(launch_session->client_uuid);
      }

      // If launched from client
      if (launch_session->client_uuid != http::unique_id) {
        auto rikey = util::from_hex_vec(get_arg(args, "rikey"), true);
        std::copy(rikey.cbegin(), rikey.cend(), std::back_inserter(launch_session->gcm_key));

        launch_session->host_audio = host_audio;

        // Encrypted RTSP is enabled with client reported corever >= 1
        auto corever = util::from_view(get_arg(args, "corever", "0"));
        if (corever >= 1) {
          launch_session->rtsp_cipher = crypto::cipher::gcm_t {
            launch_session->gcm_key,
            false
          };
          launch_session->rtsp_iv_counter = 0;
        }
        launch_session->rtsp_url_scheme = launch_session->rtsp_cipher ? "rtspenc://"s : "rtsp://"s;

        // Generate the unique identifiers for this connection that we will send later during RTSP handshake
        unsigned char raw_payload[8];
        RAND_bytes(raw_payload, sizeof(raw_payload));
        launch_session->av_ping_payload = util::hex_vec(raw_payload);
        RAND_bytes((unsigned char *) &launch_session->control_connect_data, sizeof(launch_session->control_connect_data));

        launch_session->iv.resize(16);
        uint32_t prepend_iv = util::endian::big<uint32_t>(util::from_view(get_arg(args, "rikeyid")));
        auto prepend_iv_p = (uint8_t *) &prepend_iv;
        std::copy(prepend_iv_p, prepend_iv_p + sizeof(prepend_iv), std::begin(launch_session->iv));
      }

      struct parsed_display_mode_t {
        int width = 0;
        int height = 0;
        std::uint32_t refresh_millihz = 0;
      };
      const auto parse_display_mode = [](const std::string &mode_text) -> std::optional<parsed_display_mode_t> {
        constexpr std::uint32_t kMinRefreshMillihz = 10'000;
        constexpr std::uint32_t kMaxRefreshMillihz = 1'000'000;
        const auto parse_unsigned = [](std::string_view value, std::uint32_t maximum, std::uint32_t &result) {
          if (value.empty()) {
            return false;
          }
          std::uint64_t parsed = 0;
          for (const char ch : value) {
            if (ch < '0' || ch > '9') {
              return false;
            }
            const auto digit = static_cast<std::uint32_t>(ch - '0');
            if (parsed > (maximum - digit) / 10) {
              return false;
            }
            parsed = parsed * 10 + digit;
          }
          result = static_cast<std::uint32_t>(parsed);
          return true;
        };

        const auto first_separator = mode_text.find('x');
        const auto second_separator = first_separator == std::string::npos ? std::string::npos : mode_text.find('x', first_separator + 1);
        if (first_separator == std::string::npos || second_separator == std::string::npos ||
            mode_text.find('x', second_separator + 1) != std::string::npos) {
          return std::nullopt;
        }

        std::uint32_t width = 0;
        std::uint32_t height = 0;
        if (!parse_unsigned(std::string_view(mode_text).substr(0, first_separator), static_cast<std::uint32_t>(std::numeric_limits<int>::max()), width) ||
            !parse_unsigned(std::string_view(mode_text).substr(first_separator + 1, second_separator - first_separator - 1), static_cast<std::uint32_t>(std::numeric_limits<int>::max()), height)) {
          return std::nullopt;
        }

        const std::string_view refresh_text {mode_text.data() + second_separator + 1, mode_text.size() - second_separator - 1};
        const auto decimal_point = refresh_text.find('.');
        std::uint32_t refresh_millihz = 0;
        if (decimal_point == std::string_view::npos) {
          std::uint32_t raw_refresh = 0;
          if (!parse_unsigned(refresh_text, kMaxRefreshMillihz, raw_refresh)) {
            return std::nullopt;
          }
          // Vibepollo's legacy mode values at or above 1000 are already millihertz.
          refresh_millihz = raw_refresh >= 1000 ? raw_refresh : raw_refresh * 1000;
        } else {
          if (refresh_text.find('.', decimal_point + 1) != std::string_view::npos) {
            return std::nullopt;
          }
          std::uint32_t whole_hertz = 0;
          std::uint32_t fractional_millihz = 0;
          const auto fractional = refresh_text.substr(decimal_point + 1);
          if (fractional.empty() || fractional.size() > 3 ||
              !parse_unsigned(refresh_text.substr(0, decimal_point), 1000, whole_hertz) ||
              !parse_unsigned(fractional, 999, fractional_millihz)) {
            return std::nullopt;
          }
          const auto scale = fractional.size() == 1 ? 100u : fractional.size() == 2 ? 10u : 1u;
          refresh_millihz = whole_hertz * 1000 + fractional_millihz * scale;
        }

        if (width == 0 || height == 0 || refresh_millihz < kMinRefreshMillihz || refresh_millihz > kMaxRefreshMillihz) {
          return std::nullopt;
        }
        return parsed_display_mode_t {
          .width = static_cast<int>(width),
          .height = static_cast<int>(height),
          .refresh_millihz = refresh_millihz,
        };
      };

      const auto requested_mode_text = get_arg(args, "mode", config::video.fallback_mode.c_str());
      if (const auto requested_mode = parse_display_mode(requested_mode_text)) {
        launch_session->width = requested_mode->width;
        launch_session->height = requested_mode->height;
        launch_session->fps = static_cast<int>(requested_mode->refresh_millihz);
      } else {
        BOOST_LOG(warning) << "Failed to parse requested display mode '" << requested_mode_text << "'; using 1920x1080x60.";
        launch_session->width = 1920;
        launch_session->height = 1080;
        launch_session->fps = 60000;
      }
      BOOST_LOG(info) << "Display mode for client ["sv << verified_client->name << "] requested to ["sv << requested_mode_text << ']';

      if (verified_client->display_mode.empty()) {
        launch_session->client_display_mode_override = false;
      } else if (const auto display_mode = parse_display_mode(verified_client->display_mode)) {
        // The per-client display mode controls the presentation/RTSS rate, but
        // does not overwrite the client-selected stream cadence.
        launch_session->width = display_mode->width;
        launch_session->height = display_mode->height;
        launch_session->client_display_mode_override = true;
        launch_session->client_display_refresh_millihz = display_mode->refresh_millihz;
        BOOST_LOG(info) << "Display mode for client ["sv << verified_client->name << "] overridden to ["sv << verified_client->display_mode << ']';
      } else {
        BOOST_LOG(warning) << "Failed to parse client display mode override: " << verified_client->display_mode;
        launch_session->client_display_mode_override = false;
      }

      if (!verified_client->virtual_display_mode_override.empty()) {
        if (const auto parsed_mode = parse_virtual_display_mode_override(verified_client->virtual_display_mode_override)) {
          launch_session->virtual_display_mode_override = *parsed_mode;
        }
      }
      if (!verified_client->virtual_display_layout_override.empty()) {
        if (const auto parsed_layout = parse_virtual_display_layout_override(verified_client->virtual_display_layout_override)) {
          launch_session->virtual_display_layout_override = *parsed_layout;
        }
      }
      launch_session->client_requests_virtual_display = verified_client->always_use_virtual_display;
      if (!verified_client->hdr_profile.empty()) {
        launch_session->hdr_profile = verified_client->hdr_profile;
      }
      launch_session->unique_id = get_arg(args, "uniqueid", "unknown");
      launch_session->perm = verified_client->perm;
      const auto launch_appid_arg = get_arg(args, "appid", "0");
      const auto launch_appuuid_arg = get_arg(args, "appuuid", "");
      auto launch_app_ctx = proc::proc.resolve_app(launch_appid_arg, launch_appuuid_arg);
      int app_scale_factor = 100;
      launch_session->appid = launch_app_ctx ? (int) util::from_view(launch_app_ctx->id) : (int) util::from_view(launch_appid_arg);
      if (!verified_client->output_name_override.empty()) {
        launch_session->output_name_override = verified_client->output_name_override;
      }

      const auto original_client_name = boost::algorithm::trim_copy(launch_session->client_name);
      if (!original_client_name.empty() && is_placeholder_client_name(original_client_name)) {
        const auto resolved_display_client_name =
          display_client_name_for_session(launch_session->client_name, launch_session->device_name, config::nvhttp.sunshine_name);
        BOOST_LOG(warning) << "Resolved paired client name '" << launch_session->client_name
                           << "' is not safe for display identity; using '" << resolved_display_client_name << "' instead.";
        launch_session->client_name = resolved_display_client_name;
      } else {
        launch_session->client_name = original_client_name;
      }

      if (launch_app_ctx || launch_session->appid > 0) {
        try {
          if (auto app_ctx = launch_app_ctx ? launch_app_ctx : proc::proc.resolve_app(launch_session->appid)) {
            launch_session->appid = (int) util::from_view(app_ctx->id);
            app_scale_factor = app_ctx->scale_factor;
            launch_session->gen1_framegen_fix = app_ctx->gen1_framegen_fix;
            launch_session->gen2_framegen_fix = app_ctx->gen2_framegen_fix;
            launch_session->frame_generation_enabled = app_ctx->frame_generation_enabled;
            launch_session->lossless_scaling_framegen = app_ctx->lossless_scaling_framegen;
            launch_session->lossless_scaling_target_fps = app_ctx->lossless_scaling_target_fps;
            launch_session->lossless_scaling_rtss_limit = app_ctx->lossless_scaling_rtss_limit;
            launch_session->frame_generation_provider = app_ctx->frame_generation_provider;
            rtsp_stream::launch_session_t::app_metadata_t metadata;
            metadata.id = app_ctx->id;
            metadata.name = app_ctx->name;
            metadata.virtual_screen = app_ctx->virtual_screen || app_ctx->virtual_display;
            metadata.has_command = !app_ctx->cmd.empty();
            metadata.has_playnite = !app_ctx->playnite_id.empty();
            metadata.playnite_fullscreen = app_ctx->playnite_fullscreen;
            launch_session->virtual_display = app_ctx->virtual_screen;
            if (!launch_session->virtual_display_mode_override && app_ctx->virtual_display_mode_override) {
              launch_session->virtual_display_mode_override = app_ctx->virtual_display_mode_override;
            }
            if (!launch_session->virtual_display_layout_override && app_ctx->virtual_display_layout_override) {
              launch_session->virtual_display_layout_override = app_ctx->virtual_display_layout_override;
            }
            if (!launch_session->dd_config_option_override && app_ctx->dd_config_option_override) {
              launch_session->dd_config_option_override = app_ctx->dd_config_option_override;
            }
            if (app_ctx->output_name_override) {
              launch_session->output_name_override = *app_ctx->output_name_override;
            }
            launch_session->app_metadata = std::move(metadata);
          }
        } catch (...) {
        }
      }

      launch_session->framegen_refresh_rate.reset();
      launch_session->framegen_refresh_millihz.reset();
      launch_session->framegen_refresh_multiplier = 1;
      launch_session->enable_sops = util::from_view(get_arg(args, "sops", "0"));
      launch_session->surround_info = util::from_view(get_arg(args, "surroundAudioInfo", "196610"));
      launch_session->surround_params = (get_arg(args, "surroundParams", ""));
      launch_session->gcmap = util::from_view(get_arg(args, "gcmap", "0"));
      launch_session->enable_hdr = util::from_view(get_arg(args, "hdrMode", "0"));
      launch_session->client_vrr_requested = util::from_view(get_arg(args, "clientVrrRequested", "0"));
      launch_session->prefer_sdr_10bit = verified_client->prefer_10bit_sdr;
#ifdef _WIN32
      {
        using override_e = config::video_t::dd_t::hdr_request_override_e;
        switch (config::video.dd.hdr_request_override) {
          case override_e::force_on:
            launch_session->enable_hdr = true;
            launch_session->prefer_sdr_10bit = false;
            launch_session->force_sdr = false;
            break;
          case override_e::force_off:
            launch_session->enable_hdr = false;
            launch_session->force_sdr = true;
            break;
          case override_e::automatic:
            break;
        }
      }
#endif
      if (const auto virtual_display_arg = args.find("virtualDisplay"); virtual_display_arg != std::end(args)) {
        launch_session->client_virtual_display_override = util::from_view(virtual_display_arg->second) != 0;
        if (!*launch_session->client_virtual_display_override) {
          launch_session->virtual_display_mode_override = config::video_t::virtual_display_mode_e::disabled;
        }
      }

      const auto client_scale_factor = util::from_view(get_arg(args, "scaleFactor", "100"));
      launch_session->scale_factor = client_scale_factor > 0 &&
                                             client_scale_factor <= std::numeric_limits<std::uint32_t>::max() ?
                                       static_cast<std::uint32_t>(client_scale_factor) :
                                       100u;
      const auto effective_scale_factor = app_scale_factor != 100 ?
                                            static_cast<std::int64_t>(app_scale_factor) :
                                            client_scale_factor;
      if (effective_scale_factor > 0 && effective_scale_factor != 100 && launch_session->width > 0 && launch_session->height > 0) {
        const auto scale_dimension = [effective_scale_factor](const int dimension) -> std::optional<int> {
          const auto unsigned_dimension = static_cast<std::uint64_t>(dimension);
          const auto unsigned_scale_factor = static_cast<std::uint64_t>(effective_scale_factor);
          if (unsigned_dimension > std::numeric_limits<std::uint64_t>::max() / unsigned_scale_factor) {
            return std::nullopt;
          }

          const auto scaled_dimension = (unsigned_dimension * unsigned_scale_factor) / 100u;
          const auto even_dimension = scaled_dimension & ~1ull;
          if (even_dimension == 0 || even_dimension > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            return std::nullopt;
          }
          return static_cast<int>(even_dimension);
        };

        if (const auto width = scale_dimension(launch_session->width), height = scale_dimension(launch_session->height); width && height) {
          launch_session->resolution_override = rtsp_stream::launch_session_t::resolution_override_t {
            .width = *width,
            .height = *height,
          };
          BOOST_LOG(info) << "Using launch resolution override " << *width << "x" << *height
                          << " for effective scale factor=" << effective_scale_factor << ".";
        } else {
          BOOST_LOG(warning) << "Ignoring invalid effective scale factor=" << effective_scale_factor
                             << " for requested mode " << launch_session->width << "x" << launch_session->height << ".";
        }
      }

      launch_session->client_do_cmds = verified_client->do_cmds;
      launch_session->client_undo_cmds = verified_client->undo_cmds;

      launch_session->input_only = input_only;

      launch_session->iv.resize(16);
      uint32_t prepend_iv = util::endian::big<uint32_t>(util::from_view(get_arg(args, "rikeyid")));
      auto prepend_iv_p = (uint8_t *) &prepend_iv;
      std::copy(prepend_iv_p, prepend_iv_p + sizeof(prepend_iv), std::begin(launch_session->iv));

#ifdef _WIN32
      {
        // Default the capture gate to "proceed"; launch/resume replace it when an
        // APPLY is dispatched so capture can wait for the helper's verification.
        std::promise<rtsp_stream::launch_session_t::display_helper_gate_status_e> gate_promise;
        gate_promise.set_value(rtsp_stream::launch_session_t::display_helper_gate_status_e::proceed);
        launch_session->display_helper_gate = gate_promise.get_future().share();
      }
#endif

      return launch_session;
    }

    std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session(
      bool host_audio,
      bool input_only,
      const args_t &args,
      const crypto::named_cert_t *named_cert_p,
      const resolved_client_identity_t *resolved_client_identity
    ) {
      verified_client_t verified_client;
      if (named_cert_p) {
        verified_client = *named_cert_p;
      }
      return make_launch_session_from_snapshot(host_audio, input_only, args, verified_client, resolved_client_identity);
    }

    void remove_session(const pair_session_t &sess) {
      map_id_sess.erase(sess.client.uniqueID);
    }

    void fail_pair(pair_session_t &sess, pt::ptree &tree, const std::string status_msg) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", status_msg);
      remove_session(sess);  // Security measure, delete the session when something went wrong and force a re-pair
      BOOST_LOG(warning) << "Pair attempt failed due to " << status_msg;
    }

    void getservercert(pair_session_t &sess, pt::ptree &tree, const std::string &pin) {
      if (sess.last_phase != PAIR_PHASE::NONE) {
        fail_pair(sess, tree, "Out of order call to getservercert");
        return;
      }
      sess.last_phase = PAIR_PHASE::GETSERVERCERT;

      if (sess.async_insert_pin.salt.size() < 32) {
        fail_pair(sess, tree, "Salt too short");
        return;
      }

      std::string_view salt_view {sess.async_insert_pin.salt.data(), 32};

      auto salt = util::from_hex<std::array<uint8_t, 16>>(salt_view, true);

      auto key = crypto::gen_aes_key(salt, pin);
      sess.cipher_key = std::make_unique<crypto::aes_t>(key);

      tree.put("root.paired", 1);
      tree.put("root.plaincert", util::hex_vec(conf_intern.servercert, true));
      tree.put("root.<xmlattr>.status_code", 200);
    }

    void clientchallenge(pair_session_t &sess, pt::ptree &tree, const std::string &challenge) {
      if (sess.last_phase != PAIR_PHASE::GETSERVERCERT) {
        fail_pair(sess, tree, "Out of order call to clientchallenge");
        return;
      }
      sess.last_phase = PAIR_PHASE::CLIENTCHALLENGE;

      if (!sess.cipher_key) {
        fail_pair(sess, tree, "Cipher key not set");
        return;
      }
      crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

      std::vector<uint8_t> decrypted;
      cipher.decrypt(challenge, decrypted);

      auto x509 = crypto::x509(conf_intern.servercert);
      auto sign = crypto::signature(x509);
      auto serversecret = crypto::rand(16);

      decrypted.insert(std::end(decrypted), std::begin(sign), std::end(sign));
      decrypted.insert(std::end(decrypted), std::begin(serversecret), std::end(serversecret));

      auto hash = crypto::hash({(char *) decrypted.data(), decrypted.size()});
      auto serverchallenge = crypto::rand(16);

      std::string plaintext;
      plaintext.reserve(hash.size() + serverchallenge.size());

      plaintext.insert(std::end(plaintext), std::begin(hash), std::end(hash));
      plaintext.insert(std::end(plaintext), std::begin(serverchallenge), std::end(serverchallenge));

      std::vector<uint8_t> encrypted;
      cipher.encrypt(plaintext, encrypted);

      sess.serversecret = std::move(serversecret);
      sess.serverchallenge = std::move(serverchallenge);

      tree.put("root.paired", 1);
      tree.put("root.challengeresponse", util::hex_vec(encrypted, true));
      tree.put("root.<xmlattr>.status_code", 200);
    }

    void serverchallengeresp(pair_session_t &sess, pt::ptree &tree, const std::string &encrypted_response) {
      if (sess.last_phase != PAIR_PHASE::CLIENTCHALLENGE) {
        fail_pair(sess, tree, "Out of order call to serverchallengeresp");
        return;
      }
      sess.last_phase = PAIR_PHASE::SERVERCHALLENGERESP;

      if (!sess.cipher_key || sess.serversecret.empty()) {
        fail_pair(sess, tree, "Cipher key or serversecret not set");
        return;
      }

      std::vector<uint8_t> decrypted;
      crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

      cipher.decrypt(encrypted_response, decrypted);

      sess.clienthash = std::move(decrypted);

      auto serversecret = sess.serversecret;
      auto sign = crypto::sign256(crypto::pkey(conf_intern.pkey), serversecret);

      serversecret.insert(std::end(serversecret), std::begin(sign), std::end(sign));

      tree.put("root.pairingsecret", util::hex_vec(serversecret, true));
      tree.put("root.paired", 1);
      tree.put("root.<xmlattr>.status_code", 200);
    }

    void clientpairingsecret(
      pair_session_t &sess,
      const std::shared_ptr<safe::queue_t<crypto::x509_t>> &pending_certs,
      pt::ptree &tree,
      const std::string &client_pairing_secret
    ) {
      if (sess.last_phase != PAIR_PHASE::SERVERCHALLENGERESP) {
        fail_pair(sess, tree, "Out of order call to clientpairingsecret");
        return;
      }
      sess.last_phase = PAIR_PHASE::CLIENTPAIRINGSECRET;

      auto &client = sess.client;

      if (client_pairing_secret.size() <= 16) {
        fail_pair(sess, tree, "Client pairing secret too short");
        return;
      }

      std::string_view secret {client_pairing_secret.data(), 16};
      std::string_view sign {client_pairing_secret.data() + secret.size(), client_pairing_secret.size() - secret.size()};

      auto x509 = crypto::x509(client.cert);
      if (!x509) {
        fail_pair(sess, tree, "Invalid client certificate");
        return;
      }
      auto x509_sign = crypto::signature(x509);

      std::string data;
      data.reserve(sess.serverchallenge.size() + x509_sign.size() + secret.size());

      data.insert(std::end(data), std::begin(sess.serverchallenge), std::end(sess.serverchallenge));
      data.insert(std::end(data), std::begin(x509_sign), std::end(x509_sign));
      data.insert(std::end(data), std::begin(secret), std::end(secret));

      auto hash = crypto::hash(data);

      // if hash not correct, probably MITM
      bool same_hash = hash.size() == sess.clienthash.size() && std::equal(hash.begin(), hash.end(), sess.clienthash.begin());
      auto verify = crypto::verify256(crypto::x509(client.cert), secret, sign);
      if (same_hash && verify) {
        tree.put("root.paired", 1);

        if (is_placeholder_client_name(client.name)) {
          BOOST_LOG(warning) << "PIN submitted with reserved client name '" << client.name << "'; refusing to pair with placeholder identity.";
          remove_session(sess);
          return;
        }

        auto named_cert_p = std::make_shared<crypto::named_cert_t>();
        named_cert_p->name = display_client_name_for_session(client.name, std::string {}, "Moonlight Client"s);
        for (char &c : named_cert_p->name) {
          if (c == '(') {
            c = '[';
          } else if (c == ')') {
            c = ']';
          }
        }
        named_cert_p->cert = std::move(client.cert);
        named_cert_p->uuid = uuid_util::uuid_t::generate().string();
        // If the device is the first one paired with the server, assign full permission.
        bool first_client = false;
        {
          std::lock_guard<std::mutex> lock(client_mutex);
          first_client = client_root.named_devices.empty();
        }
        if (first_client) {
          named_cert_p->perm = PERM::_all;
        } else {
          named_cert_p->perm = PERM::_default;
        }

        named_cert_p->enable_legacy_ordering = true;
        named_cert_p->allow_client_commands = true;
        named_cert_p->always_use_virtual_display = false;
        named_cert_p->output_name_override.clear();

        add_authorized_client(named_cert_p);

        if (pending_certs) {
          pending_certs->raise(crypto::x509(named_cert_p->cert));
        }
      } else {
        tree.put("root.paired", 0);
        BOOST_LOG(warning) << "Pair attempt failed due to same_hash: " << same_hash << ", verify: " << verify;
      }

      remove_session(sess);
      tree.put("root.<xmlattr>.status_code", 200);
    }

    void clientpairingsecret(pair_session_t &sess, pt::ptree &tree, const std::string &client_pairing_secret) {
      clientpairingsecret(sess, pending_cert_queue, tree, client_pairing_secret);
    }

    template<class T>
    struct tunnel;

    template<>
    struct tunnel<SunshineHTTPS> {
      static auto constexpr to_string = "HTTPS"sv;
    };

    template<>
    struct tunnel<SimpleWeb::HTTP> {
      static auto constexpr to_string = "NONE"sv;
    };

    inline verified_client_t get_verified_cert(req_https_t request) {
      if (auto remembered = get_remembered_tls_client_identity(request)) {
        std::lock_guard<std::mutex> lock(client_mutex);
        for (const auto &named_cert_p : client_root.named_devices) {
          if (named_cert_p && named_cert_p->uuid == remembered->uuid) {
            return *named_cert_p;
          }
        }
      }

      if (!tl_peer_certificate) {
        return std::nullopt;
      }

      const auto peer_signature = crypto::signature(tl_peer_certificate.get());
      std::lock_guard<std::mutex> lock(client_mutex);
      for (const auto &named_cert_p : client_root.named_devices) {
        if (!named_cert_p) {
          continue;
        }
        auto stored_x509 = crypto::x509(named_cert_p->cert);
        if (stored_x509 && crypto::signature(stored_x509.get()) == peer_signature) {
          return *named_cert_p;
        }
      }
      return std::nullopt;
    }

    inline PERM client_perm(const verified_client_t &verified_client) {
      return verified_client ? verified_client->perm : PERM::_no;
    }

    inline bool has_client_perm(const verified_client_t &verified_client, PERM perm) {
      return !!(client_perm(verified_client) & perm);
    }

    // `quiet` keeps high-frequency background polls (e.g. ServerCommand) at debug level
    // so a client that legitimately lacks the permission doesn't flood the log.
    inline void log_permission_denied(std::string_view action, std::string_view perm_label, const verified_client_t &verified_client, bool quiet = false) {
      const auto perm = client_perm(verified_client);
      auto &log_target = quiet ? debug : warning;
      if (verified_client) {
        BOOST_LOG(log_target) << "Permission " << action << " denied for client [" << verified_client->name << "]: it lacks the \"" << perm_label
                              << "\" permission (current permission mask 0x" << std::hex << (uint32_t) perm << std::dec
                              << "). Grant it in the Web UI under Client Management.";
      } else {
        BOOST_LOG(log_target) << "Permission " << action << " denied: HTTPS client certificate not recognized. The device may need to be re-paired.";
      }
    }

    // Sent to Moonlight as the launch/resume/cancel status_message, which most clients
    // show verbatim in their error dialog, so it has to explain the fix rather than just fail.
    inline std::string permission_denied_status_message(const verified_client_t &verified_client, std::string_view perm_label) {
      if (!verified_client) {
        return "Permission denied: this device's certificate is not recognized by the host. Unpair and pair this device again.";
      }
      std::string msg;
      msg.reserve(160);
      msg += "Permission denied: this device lacks the \"";
      msg += perm_label;
      msg += "\" permission. Enable it on the host in the Vibepollo Web UI under Client Management.";
      return msg;
    }

    template<class T>
    void print_req(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
      BOOST_LOG(verbose) << "HTTP "sv << request->method << ' ' << request->path << " tunnel="sv << tunnel<T>::to_string;

      if (!request->header.empty()) {
        BOOST_LOG(verbose) << "Headers:"sv;
        for (auto &[name, val] : request->header) {
          BOOST_LOG(verbose) << name << " -- " << val;
        }
      }

      auto query = request->parse_query_string();
      if (!query.empty()) {
        BOOST_LOG(verbose) << "Query Params:"sv;
        for (auto &[name, val] : query) {
          BOOST_LOG(verbose) << name << " -- " << val;
        }
      }
    }


    template<class T>
    void not_found(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
      print_req<T>(request);

      pt::ptree tree;
      tree.put("root.<xmlattr>.status_code", 404);

      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(SimpleWeb::StatusCode::client_error_not_found, data.str());
      response->close_connection_after_response = true;

    }

    template<class T>
    void unpair(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
      print_req<T>(request);

      pt::ptree tree;

      auto fg = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_xml(data, tree);
        response->write(data.str());
        response->close_connection_after_response = true;
      });

      auto args = request->parse_query_string();
      auto unique_id = get_arg(args, "uniqueid", "");

      const bool cleaned_pending_pair = !unique_id.empty() && map_id_sess.erase(unique_id) > 0;
      bool removed = false;

      if constexpr (std::is_same_v<T, SunshineHTTPS>) {
        if (auto verified_client = get_verified_cert(request)) {
          removed = unpair_client(verified_client->uuid);
        }
      }

      tree.put("root.unpaired", removed ? 1 : 0);
      tree.put("root.<xmlattr>.status_code", 200);

      if (cleaned_pending_pair) {
        BOOST_LOG(info) << "Cleaned pending pairing session during unpair request";
      }
    }

    template<class T>
    void pair(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
      print_req<T>(request);

      pt::ptree tree;

      auto fg = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_xml(data, tree);
        response->write(data.str());
        response->close_connection_after_response = true;
      });

      if (!config::sunshine.enable_pairing) {
        tree.put("root.<xmlattr>.status_code", 403);
        tree.put("root.<xmlattr>.status_message", "Pairing is disabled for this instance");

        return;
      }

      auto args = request->parse_query_string();
      if (args.find("uniqueid"s) == std::end(args)) {
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "Missing uniqueid parameter");

        return;
      }

      auto uniqID {get_arg(args, "uniqueid")};

      args_t::const_iterator it;
      if (it = args.find("phrase"); it != std::end(args)) {
        if (it->second == "getservercert"sv) {
          pair_session_t sess;

          auto deviceName {get_arg(args, "devicename")};

          if (deviceName == "roth"sv) {
            deviceName = "Legacy Moonlight Client";
          }

          sess.client.uniqueID = std::move(uniqID);
          sess.client.name = std::move(deviceName);
          sess.client.cert = util::from_hex_vec(get_arg(args, "clientcert"), true);

          BOOST_LOG(verbose) << sess.client.cert;
          auto session_id = sess.client.uniqueID;
          if (auto existing = map_id_sess.find(session_id); existing != map_id_sess.end()) {
            BOOST_LOG(info) << "Replacing stale pending pairing session for uniqueid=" << session_id;
            map_id_sess.erase(existing);
          }
          auto ptr = map_id_sess.emplace(std::move(session_id), std::move(sess)).first;

          ptr->second.async_insert_pin.salt = std::move(get_arg(args, "salt"));

          auto it = args.find("otpauth");
          if (it != std::end(args)) {
            if (one_time_pin.empty() || (std::chrono::steady_clock::now() - otp_creation_time > OTP_EXPIRE_DURATION)) {
              one_time_pin.clear();
              otp_passphrase.clear();
              otp_device_name.clear();
              tree.put("root.<xmlattr>.status_code", 503);
              tree.put("root.<xmlattr>.status_message", "OTP auth not available.");
            } else {
              auto hash = util::hex(crypto::hash(one_time_pin + ptr->second.async_insert_pin.salt + otp_passphrase), true);

              if (hash.to_string_view() == it->second) {
                if (!otp_device_name.empty()) {
                  ptr->second.client.name = std::move(otp_device_name);
                }

                getservercert(ptr->second, tree, one_time_pin);

                one_time_pin.clear();
                otp_passphrase.clear();
                otp_device_name.clear();
                return;
              }
            }

            // Always return positive, attackers will fail in the next steps.
            getservercert(ptr->second, tree, crypto::rand(16));
            return;
          }

          if (config::sunshine.flags[config::flag::PIN_STDIN]) {
            std::string pin;

            std::cout << "Please insert pin: "sv;
            std::getline(std::cin, pin);

            getservercert(ptr->second, tree, pin);
          } else {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
            system_tray::update_tray_require_pin();
#endif
            ptr->second.async_insert_pin.response = std::move(response);

            fg.disable();
            return;
          }
        } else if (it->second == "pairchallenge"sv) {
          tree.put("root.paired", 1);
          tree.put("root.<xmlattr>.status_code", 200);
          return;
        }
      }

      auto sess_it = map_id_sess.find(uniqID);
      if (sess_it == std::end(map_id_sess)) {
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "Invalid uniqueid");
        return;
      }


      if (it = args.find("clientchallenge"); it != std::end(args)) {
        auto challenge = util::from_hex_vec(it->second, true);
        clientchallenge(sess_it->second, tree, challenge);
      } else if (it = args.find("serverchallengeresp"); it != std::end(args)) {
        auto encrypted_response = util::from_hex_vec(it->second, true);
        serverchallengeresp(sess_it->second, tree, encrypted_response);
      } else if (it = args.find("clientpairingsecret"); it != std::end(args)) {
        auto pairingsecret = util::from_hex_vec(it->second, true);
        clientpairingsecret(sess_it->second, tree, pairingsecret);
      } else {
        tree.put("root.<xmlattr>.status_code", 404);
        tree.put("root.<xmlattr>.status_message", "Invalid pairing request");
      }
    }

    bool pin(std::string pin, std::string name) {
      pt::ptree tree;
      if (map_id_sess.empty()) {
        BOOST_LOG(warning) << "PIN submitted but no pending pairing session exists";
        return false;
      }

      // ensure pin is 4 digits
      if (pin.size() != 4) {
        tree.put("root.paired", 0);
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put(
          "root.<xmlattr>.status_message",
          std::format("Pin must be 4 digits, {} provided", pin.size())
        );
        return false;
      }

      // ensure all pin characters are numeric
      if (!std::all_of(pin.begin(), pin.end(), ::isdigit)) {
        tree.put("root.paired", 0);
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "Pin must be numeric");
        return false;
      }

      const auto now = std::chrono::steady_clock::now();
      constexpr auto pairing_session_expiry = std::chrono::minutes(10);
      std::erase_if(map_id_sess, [now, pairing_session_expiry](const auto &entry) {
        const auto &sess = entry.second;
        return sess.last_phase == PAIR_PHASE::NONE && now - sess.created_at > pairing_session_expiry;
      });

      auto sess_it = map_id_sess.end();
      for (auto it = map_id_sess.begin(); it != map_id_sess.end(); ++it) {
        if (it->second.last_phase != PAIR_PHASE::NONE) {
          continue;
        }
        if (sess_it == map_id_sess.end() || sess_it->second.created_at < it->second.created_at) {
          sess_it = it;
        }
      }

      if (sess_it == map_id_sess.end()) {
        BOOST_LOG(warning) << "PIN submitted but no active pending pairing session is ready";
        return false;
      }

      auto &sess = sess_it->second;
      if (sess.async_insert_pin.salt.size() < 32) {
        BOOST_LOG(warning) << "PIN submitted but pending pairing session has an invalid salt";
        remove_session(sess);
        return false;
      }

      getservercert(sess, tree, pin);

      if (!name.empty()) {
        sess.client.name = name;
      }

      // response to the request for pin
      std::ostringstream data;
      pt::write_xml(data, tree);

      auto &async_response = sess.async_insert_pin.response;
      // Keep Content-Length on this delayed response; Moonlight waits for a complete body.
      if (async_response.has_left() && async_response.left()) {
        async_response.left()->write(data.str());
      } else if (async_response.has_right() && async_response.right()) {
        async_response.right()->write(data.str());
      } else {
        BOOST_LOG(warning) << "PIN submitted but pending pairing session has no response channel";
        remove_session(sess);
        return false;
      }

      // reset async_response
      async_response = std::decay_t<decltype(async_response.left())>();
      // response to the current request
      return true;
    }

    template<class T>
    void serverinfo(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
      print_req<T>(request);

      int pair_status = 0;
      if constexpr (std::is_same_v<SunshineHTTPS, T>) {
        auto args = request->parse_query_string();
        auto clientID = args.find("uniqueid"s);

        if (clientID != std::end(args)) {
          pair_status = 1;
        }
      }

      auto local_endpoint = request->local_endpoint();

      pt::ptree tree;

      tree.put("root.<xmlattr>.status_code", 200);
      tree.put("root.hostname", config::nvhttp.sunshine_name);

      tree.put("root.appversion", VERSION);
      tree.put("root.GfeVersion", GFE_VERSION);
      tree.put("root.uniqueid", http::unique_id);
      tree.put("root.HttpsPort", net::map_port(PORT_HTTPS));
      tree.put("root.ExternalPort", net::map_port(PORT_HTTP));
#ifdef _WIN32
      // Artemis checks /serverinfo before it offers virtual-display launches.
      // Publish the Windows capability and its current driver state for both
      // paired and unpaired discovery requests.
      tree.put("root.VirtualDisplayCapable", true);
      tree.put("root.VirtualDisplayDriverReady", proc::vDisplayDriverStatus.load(std::memory_order_acquire) == VDISPLAY::DRIVER_STATUS::OK);
#else
      tree.put("root.VirtualDisplayCapable", false);
      tree.put("root.VirtualDisplayDriverReady", false);
#endif

      // Only include the MAC address for requests sent from paired clients over HTTPS.
      // For HTTP requests, use a placeholder MAC address that Moonlight knows to ignore.
      if constexpr (std::is_same_v<SunshineHTTPS, T>) {
        tree.put("root.mac", platf::get_mac_address(net::addr_to_normalized_string(local_endpoint.address())));

        auto verified_client = get_verified_cert(request);
        const auto perm = client_perm(verified_client);

        if (!!(perm & PERM::server_cmd)) {
          pt::ptree &root_node = tree.get_child("root");

          if (config::sunshine.server_cmds.size() > 0) {
            // Broadcast server_cmds
            for (const auto &cmd : config::sunshine.server_cmds) {
              pt::ptree cmd_node;
              cmd_node.put_value(cmd.cmd_name);
              root_node.push_back(std::make_pair("ServerCommand", cmd_node));
            }
          }
        } else {
          log_permission_denied("Get ServerCommand"sv, "Run server commands"sv, verified_client, true);
        }

        tree.put("root.Permission", std::to_string((uint32_t) perm));
      } else {
        tree.put("root.mac", "00:00:00:00:00:00");
        tree.put("root.Permission", "0");
      }

      // Moonlight clients track LAN IPv6 addresses separately from LocalIP which is expected to
      // always be an IPv4 address. If we return that same IPv6 address here, it will clobber the
      // stored LAN IPv4 address. To avoid this, we need to return an IPv4 address in this field
      // when we get a request over IPv6.
      //
      // HACK: We should return the IPv4 address of local interface here, but we don't currently
      // have that implemented. For now, we will emulate the behavior of GFE+GS-IPv6-Forwarder,
      // which returns 127.0.0.1 as LocalIP for IPv6 connections. Moonlight clients with IPv6
      // support know to ignore this bogus address.
      if (local_endpoint.address().is_v6() && !local_endpoint.address().to_v6().is_v4_mapped()) {
        tree.put("root.LocalIP", "127.0.0.1");
      } else {
        tree.put("root.LocalIP", net::addr_to_normalized_string(local_endpoint.address()));
      }

#ifdef _WIN32
      const auto advertised_video = advertised_encoder_capabilities_for_http().advertised;
#else
      const auto advertised_video = video::advertised_encoder_capabilities(true);
#endif

      tree.put("root.MaxLumaPixelsHEVC", advertised_video.hevc_mode > 1 ? "1869449984" : "0");

      uint32_t codec_mode_flags = SCM_H264;
      if (advertised_video.yuv444_for_codec[0]) {
        codec_mode_flags |= SCM_H264_HIGH8_444;
      }
      if (advertised_video.hevc_mode >= 2) {
        codec_mode_flags |= SCM_HEVC;
        if (advertised_video.yuv444_for_codec[1]) {
          codec_mode_flags |= SCM_HEVC_REXT8_444;
        }
      }
      if (advertised_video.hevc_mode >= 3) {
        codec_mode_flags |= SCM_HEVC_MAIN10;
        if (advertised_video.yuv444_for_codec[1]) {
          codec_mode_flags |= SCM_HEVC_REXT10_444;
        }
      }
      if (advertised_video.av1_mode >= 2) {
        codec_mode_flags |= SCM_AV1_MAIN8;
        if (advertised_video.yuv444_for_codec[2]) {
          codec_mode_flags |= SCM_AV1_HIGH8_444;
        }
      }
      if (advertised_video.av1_mode >= 3) {
        codec_mode_flags |= SCM_AV1_MAIN10;
        if (advertised_video.yuv444_for_codec[2]) {
          codec_mode_flags |= SCM_AV1_HIGH10_444;
        }
      }
      tree.put("root.ServerCodecModeSupport", codec_mode_flags);

      tree.put("root.PairStatus", pair_status);

      if constexpr (std::is_same_v<SunshineHTTPS, T>) {
        int current_appid = proc::proc.running();
        // When input only mode is enabled, the only resume method should be launching the same app again.
        if (config::input.enable_input_only_mode && current_appid != proc::input_only_app_id) {
          current_appid = 0;
        }
        tree.put("root.currentgame", current_appid);
        tree.put("root.currentgameuuid", proc::proc.get_running_app_uuid());
        tree.put("root.state", current_appid > 0 ? "SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE");
      } else {
        tree.put("root.currentgame", 0);
        tree.put("root.currentgameuuid", "");
        tree.put("root.state", "SUNSHINE_SERVER_FREE");
      }

      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    }

    bool get_client_prefer_10bit_sdr(const std::string &uuid) {
      std::lock_guard<std::mutex> lock(client_mutex);
      for (auto &named_cert : client_root.named_devices) {
        if (named_cert->uuid == uuid) {
          return named_cert->prefer_10bit_sdr;
        }
      }
      return false;
    }

    verified_client_t get_client_snapshot_by_uuid(const std::string &uuid) {
      if (uuid.empty()) {
        return std::nullopt;
      }

      std::lock_guard<std::mutex> lock(client_mutex);
      for (auto &named_cert : client_root.named_devices) {
        if (named_cert->uuid == uuid) {
          return *named_cert;
        }
      }
      return std::nullopt;
    }

    nlohmann::json get_all_clients() {
      nlohmann::json named_cert_nodes = nlohmann::json::array();
      const client_t client = client_root_snapshot();
      std::list<std::string> connected_uuids = rtsp_stream::get_all_session_uuids();

      for (auto &named_cert : client.named_devices) {
        nlohmann::json named_cert_node;
        named_cert_node["name"] = named_cert->name;
        named_cert_node["uuid"] = named_cert->uuid;
        named_cert_node["display_mode"] = named_cert->display_mode;
        if (!named_cert->hdr_profile.empty()) {
          named_cert_node["hdr_profile"] = named_cert->hdr_profile;
        }
        named_cert_node["output_name_override"] = named_cert->output_name_override;
        named_cert_node["virtual_display_mode"] = named_cert->virtual_display_mode_override;
        named_cert_node["virtual_display_layout"] = named_cert->virtual_display_layout_override;
        named_cert_node["perm"] = static_cast<uint32_t>(named_cert->perm);
        named_cert_node["enable_legacy_ordering"] = named_cert->enable_legacy_ordering;
        named_cert_node["allow_client_commands"] = named_cert->allow_client_commands;
        named_cert_node["always_use_virtual_display"] = named_cert->always_use_virtual_display;
        named_cert_node["prefer_10bit_sdr"] = named_cert->prefer_10bit_sdr;
        if (named_cert->last_seen.has_value()) {
          named_cert_node["last_seen"] = *named_cert->last_seen;
        }
        if (!named_cert->config_overrides.empty()) {
          named_cert_node["config_overrides"] = named_cert->config_overrides;
        }

        // Add "do" commands if available
        if (!named_cert->do_cmds.empty()) {
          nlohmann::json do_cmds_node = nlohmann::json::array();
          for (const auto &cmd : named_cert->do_cmds) {
            do_cmds_node.push_back(crypto::command_entry_t::serialize(cmd));
          }
          named_cert_node["do"] = do_cmds_node;
        }

        // Add "undo" commands if available
        if (!named_cert->undo_cmds.empty()) {
          nlohmann::json undo_cmds_node = nlohmann::json::array();
          for (const auto &cmd : named_cert->undo_cmds) {
            undo_cmds_node.push_back(crypto::command_entry_t::serialize(cmd));
          }
          named_cert_node["undo"] = undo_cmds_node;
        }

        // Determine connection status
        bool connected = false;
        if (connected_uuids.empty()) {
          connected = false;
        } else {
          for (auto it = connected_uuids.begin(); it != connected_uuids.end(); ++it) {
            if (*it == named_cert->uuid) {
              connected = true;
              connected_uuids.erase(it);
              break;
            }
          }
        }
        named_cert_node["connected"] = connected;

        named_cert_nodes.push_back(named_cert_node);
      }

      return named_cert_nodes;
    }

    void mark_client_last_seen(const std::string &uuid) {
      if (uuid.empty()) {
        return;
      }

      bool changed = false;
      {
        std::lock_guard<std::mutex> lock(client_mutex);
        for (auto &named_cert : client_root.named_devices) {
          if (named_cert->uuid != uuid) {
            continue;
          }

          const auto now = now_seconds();
          if (named_cert->last_seen.has_value() && *named_cert->last_seen == now) {
            return;
          }
          named_cert->last_seen = now;
          changed = true;
          break;
        }
      }
      if (changed && !config::sunshine.flags[config::flag::FRESH_STATE]) {
        save_state();
      }
    }

    void applist(resp_https_t response, req_https_t request) {
      print_req<SunshineHTTPS>(request);

      pt::ptree tree;

      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_xml(data, tree);
        response->write(data.str());
        response->close_connection_after_response = true;
      });

      auto &apps = tree.add_child("root", pt::ptree {});

      apps.put("<xmlattr>.status_code", 200);

      auto verified_client = get_verified_cert(request);
      if (has_client_perm(verified_client, PERM::_all_actions)) {
        auto current_appid = proc::proc.running();
        // Only expose the special "Terminate" entry (and the "busy minimal list" behavior)
        // when input-only mode is enabled. Otherwise, Moonlight handles terminate/resume UI
        // without needing a fake app entry in the list.
        const bool show_terminate_entry =
          config::input.enable_input_only_mode && current_appid > 0 && current_appid != proc::input_only_app_id;
        const bool should_hide_inactive_apps = show_terminate_entry;

        auto app_list = proc::proc.get_apps();

        std::vector<const proc::ctx_t *> visible_apps;
        visible_apps.reserve(app_list.size());

        for (const auto &app : app_list) {
          auto appid = util::from_view(app.id);
          bool include = true;
          if (should_hide_inactive_apps) {
            if (
              appid != current_appid && appid != proc::input_only_app_id && appid != proc::terminate_app_id
            ) {
              include = false;
            }
          } else if (appid == proc::terminate_app_id) {
            include = show_terminate_entry;
          }

          if (!include) {
            continue;
          }

          visible_apps.push_back(&app);
        }

        const bool enable_legacy_ordering = config::sunshine.legacy_ordering && verified_client->enable_legacy_ordering;
        size_t bits = 0;
        if (enable_legacy_ordering && !visible_apps.empty()) {
          bits = zwpad::pad_width_for_count(visible_apps.size());
        }

#ifdef _WIN32
        const auto advertised_video = advertised_encoder_capabilities_for_http().advertised;
#else
        const auto advertised_video = video::advertised_encoder_capabilities(true);
#endif
        const bool is_hdr_supported = advertised_video.hevc_mode == 3 || advertised_video.av1_mode == 3;

        for (size_t i = 0; i < visible_apps.size(); ++i) {
          const auto &app = *visible_apps[i];

          std::string app_name;
          if (enable_legacy_ordering && bits > 0) {
            app_name = zwpad::pad_for_ordering(app.name, bits, i);
          } else {
            app_name = app.name;
          }

          pt::ptree app_node;

          app_node.put("IsHdrSupported"s, is_hdr_supported ? 1 : 0);
          app_node.put("AppTitle"s, app_name);
          app_node.put("UUID", app.uuid);
          app_node.put("IDX", app.idx);
          app_node.put("ID", app.id);
          app_node.put("ArtVersion", app.art_version);

          apps.push_back(std::make_pair("App", std::move(app_node)));
        }
      } else {
        log_permission_denied("ListApp"sv, "List applications"sv, verified_client);

        pt::ptree app_node;

        app_node.put("IsHdrSupported"s, 0);
        app_node.put("AppTitle"s, "Permission denied - enable \"List applications\" for this device in the host's Web UI");
        app_node.put("UUID", "");
        app_node.put("IDX", "0");
        app_node.put("ID", "114514");

        apps.push_back(std::make_pair("App", std::move(app_node)));

        return;
      }
    }

    void launch(bool &host_audio, resp_https_t response, req_https_t request, int current_appid) {
      print_req<SunshineHTTPS>(request);

#ifdef _WIN32
      // Keep encoder probes blocked across the complete failure unwind: virtual
      // display removal, response publication, and any final helper restore.
      stream::session::cleanup_reservation_t cleanup_reservation;
#endif
      pt::ptree tree;
      bool revert_display_configuration = false;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_xml(data, tree);
        response->write(data.str());
        response->close_connection_after_response = true;

        if (revert_display_configuration) {
          display_helper_integration::revert();
        }
      });

      auto args = request->parse_query_string();

      auto appid_str = get_arg(args, "appid", "0");
      auto appuuid_str = get_arg(args, "appuuid", "");
      auto requested_app = proc::proc.resolve_app(appid_str, appuuid_str);
      auto appid = requested_app ? util::from_view(requested_app->id) : util::from_view(appid_str);
      auto current_app_uuid = proc::proc.get_running_app_uuid();
      bool is_input_only = config::input.enable_input_only_mode && (appid == proc::input_only_app_id || (appuuid_str == REMOTE_INPUT_UUID));

      auto verified_client = get_verified_cert(request);
      const auto request_client_identity = resolve_client_identity(request, verified_client);
      auto required_perm = PERM::launch;

      BOOST_LOG(verbose) << "Launching app [" << appid_str << "] with UUID [" << appuuid_str << "]";
      // BOOST_LOG(verbose) << "QS: " << request->query_string;

      // If we have already launched an app, we should allow clients with view permission to join the input only or current app's session.
      if (
        current_appid > 0 && (appuuid_str != TERMINATE_APP_UUID || appid != proc::terminate_app_id) && (is_input_only || appid == current_appid || (!appuuid_str.empty() && appuuid_str == current_app_uuid))
      ) {
        required_perm = PERM::_allow_view;
      }

      if (!has_client_perm(verified_client, required_perm)) {
        const auto perm_label = required_perm == PERM::launch ? "Launch applications"sv : "View stream"sv;
        log_permission_denied("LaunchApp"sv, perm_label, verified_client);

        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 403);
        tree.put("root.<xmlattr>.status_message", permission_denied_status_message(verified_client, perm_label));

        return;
      }
      if (
        args.find("rikey"s) == std::end(args) ||
        args.find("rikeyid"s) == std::end(args) ||
        args.find("localAudioPlayMode"s) == std::end(args) ||
        (args.find("appid"s) == std::end(args) && args.find("appuuid"s) == std::end(args))
      ) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "Missing a required launch parameter");

        return;
      }

      if (!is_input_only) {
        // Special handling for the "terminate" app
        if (
          (appid == proc::terminate_app_id && proc::terminate_app_id > 0) || appuuid_str == TERMINATE_APP_UUID
        ) {
          proc::proc.terminate(false, true, false, true);

          tree.put("root.resume", 0);
          tree.put("root.<xmlattr>.status_code", 410);
          tree.put("root.<xmlattr>.status_message", "App terminated.");


          return;
        }

        if (
          current_appid > 0 && current_appid != proc::input_only_app_id && ((appid > 0 && appid != current_appid) || (!appuuid_str.empty() && appuuid_str != current_app_uuid))
        ) {
          tree.put("root.resume", 0);
          tree.put("root.<xmlattr>.status_code", 400);
          tree.put("root.<xmlattr>.status_message", "An app is already running on this host");

          return;
        }
      }

      if (rtsp_stream::has_pending_launch_or_startup()) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "Another RTSP session launch is pending");
        return;
      }

      host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));

      bool no_active_sessions = !has_stream_session_activity();
      // Runtime overrides are global process state. Do not reapply them while
      // another RTSP/WebRTC session is active, otherwise a second client can mutate
      // active stream limits (e.g. fps/encoding-related settings) mid-session.
      const bool update_runtime_overrides = no_active_sessions;

      // Build the requested layer even when another stream owns the process-wide
      // runtime config. A shared capture may only be joined when its adapter pair
      // is compatible with the one that is already active.
      std::unordered_map<std::string, std::string> requested_runtime_overrides;
      if (requested_app) {
        config::merge_config_overrides(requested_runtime_overrides, requested_app->config_overrides);
      }

      auto client_settings = verified_client;
      std::string client_uuid = request_client_identity.uuid;
      const auto launch_client_uuid = resolve_known_client_uuid_from_launch_id(get_arg(args, "uniqueid", ""));
      if (client_uuid.empty()) {
        client_uuid = launch_client_uuid;
      } else if (!launch_client_uuid.empty() && is_placeholder_client_name(request_client_identity.name)) {
        BOOST_LOG(warning) << "Ignoring placeholder TLS client identity '" << request_client_identity.name
                           << "' for runtime overrides; using launch uniqueid " << launch_client_uuid << ".";
        client_uuid = launch_client_uuid;
        client_settings.reset();
      }
      if (!client_settings && !client_uuid.empty()) {
        client_settings = get_client_snapshot_by_uuid(client_uuid);
      }
      if (client_settings) {
        config::merge_config_overrides(requested_runtime_overrides, client_settings->config_overrides);
      }

      if (!update_runtime_overrides &&
          !config::adapter_config_overrides_compatible_with_active(requested_runtime_overrides)) {
        BOOST_LOG(warning) << "Rejecting shared launch with a capture adapter selection that differs from the active stream.";
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put(
          "root.<xmlattr>.status_message",
          "Another stream is active with a different capture adapter selection"
        );
        return;
      }

      // Apply per-application runtime config overrides before we build session metadata or
      // prepare display/capture so the effective config is used everywhere.
      bool runtime_overrides_applied = false;
      bool keep_runtime_overrides = false;
      auto runtime_overrides_guard = util::fail_guard([&]() {
        if (!runtime_overrides_applied || keep_runtime_overrides) {
          return;
        }

        config::clear_runtime_config_overrides();

        // Restore global config immediately when safe; otherwise defer.
        if (!has_stream_session_activity()) {
          config::apply_config_now();
        } else {
          config::mark_deferred_reload();
        }
      });

      if (update_runtime_overrides) {
        try {
          auto overrides = requested_runtime_overrides;

#ifdef _WIN32
          // "Auto" client peak brightness follows the selected Windows HDR calibration
          // profile's MHC2 peak. An explicit app/client override remains authoritative.
          if (client_settings &&
              !client_settings->hdr_profile.empty() &&
              !overrides.contains("rtx_hdr_peak_brightness")) {
            if (const auto profile_peak = VDISPLAY::hdr_profile_peak_luminance_nits(client_settings->hdr_profile)) {
              const auto effective_peak = std::clamp<std::uint32_t>(*profile_peak, 400, 2000);
              overrides.insert_or_assign("rtx_hdr_peak_brightness", std::to_string(effective_peak));
              BOOST_LOG(info) << "HDR peak: using " << effective_peak << " nits from MHC2 profile '"
                              << client_settings->hdr_profile << "'"
                              << (*profile_peak == effective_peak ? "." : " (clamped to supported range).");
            } else {
              BOOST_LOG(warning) << "HDR peak: profile '" << client_settings->hdr_profile
                                 << "' has no readable MHC2 peak; using the configured default.";
            }
          }
#endif

          config::set_runtime_config_overrides(std::move(overrides));
          runtime_overrides_applied = true;

          // Re-apply config so overrides take effect in config::video/config::input/etc.
          config::apply_config_now();
        } catch (...) {
          // If something goes wrong, fall back to global config only.
          config::clear_runtime_config_overrides();
          config::apply_config_now();
          runtime_overrides_applied = true;
        }
      } else {
        BOOST_LOG(debug) << "Launch while an RTSP/WebRTC session is already active; preserving current runtime overrides.";
      }

      // Prevent interleaving with hot-apply while we prep/start a session.
      auto _hot_apply_gate = config::acquire_apply_read_gate();
      if (no_active_sessions) {
        config::record_active_adapter_config();
      }
#ifdef _WIN32
      const auto display_startup_deadline =
        std::chrono::steady_clock::now() +
        display_helper_integration::kStreamStartApplyVerificationTimeout;
      const auto display_startup_cancelled = [display_startup_deadline] {
        return std::chrono::steady_clock::now() >= display_startup_deadline;
      };
      // First step on stream start: stop any in-flight helper restore loop immediately.
      // This must happen before any other display helper work to prevent restore/crash loops on virtual displays.
      (void) display_helper_integration::disarm_pending_restore(
        display_startup_cancelled,
        display_startup_deadline
      );
#endif
      const bool allow_display_changes = true;
      auto launch_session = make_launch_session_from_snapshot(host_audio, is_input_only, args, verified_client, &request_client_identity);
      std::optional<std::string> pending_output_override;
      auto output_override_guard = util::fail_guard([&]() {
        if (pending_output_override) {
          config::set_runtime_output_name_override(std::nullopt);
        }
      });
      no_active_sessions = !has_stream_session_activity();
      if (no_active_sessions) {
        config::set_runtime_output_name_override(std::nullopt);
      }

#ifdef _WIN32
      std::optional<video::encoder_probe_adapter_hint_lease_t> pending_adapter_hint;
      auto pending_adapter_hint_guard = util::fail_guard([&] {
        if (pending_adapter_hint) {
          (void) video::clear_pending_virtual_display_adapter_hint(*pending_adapter_hint);
        }
      });
      prepare_virtual_display_for_session(
        launch_session,
        no_active_sessions,
        allow_display_changes,
        is_input_only,
        pending_output_override,
        pending_adapter_hint,
        display_startup_cancelled,
        display_startup_deadline
      );

      auto virtual_display_teardown_guard = util::fail_guard([&]() {
        stream::session::cleanup_reservation_t cleanup_reservation;
        if (has_stream_session_activity()) {
          return;
        }

        if (!launch_session->virtual_display) {
          return;
        }

        BOOST_LOG(info) << "Launch aborted before session start; removing virtual displays.";
        (void) platf::virtual_display_cleanup::run(
          "launch_aborted",
          config::video.dd.config_revert_on_disconnect
        );
      });
#endif

      // The display should be restored in case something fails as there are no other sessions.
      if (no_active_sessions && !launch_session->input_only) {
        revert_display_configuration = true;


#ifdef _WIN32
        const bool helper_session_available = display_helper_session_available();
        (void) display_helper_integration::disarm_pending_restore(
          display_startup_cancelled,
          display_startup_deadline
        );
        auto request = display_helper_integration::helpers::build_request_from_session(config::video, *launch_session);
        if (!request) {
          BOOST_LOG(warning) << "Display helper: failed to build display configuration request; continuing with existing display.";
        }

      if (request) {
        display_helper_integration::ApplyVerificationTicket verification_ticket;
        const bool applied = display_helper_integration::apply(
          *request,
          &verification_ticket,
          display_startup_cancelled,
          display_helper_integration::ApplyRetryPolicy::StreamStart,
          display_startup_deadline);
        launch_session->display_config_preapplied = applied;
        if (!applied) {
          if (helper_session_available) {
            BOOST_LOG(warning) << "Display helper: failed to apply display configuration; continuing with existing display.";
          }
        } else {
          auto gate_promise = std::make_shared<std::promise<rtsp_stream::launch_session_t::display_helper_gate_status_e>>();
          launch_session->display_helper_gate = gate_promise->get_future().share();
          BOOST_LOG(debug) << "Display helper: gating capture start on helper verification (non-blocking session start).";
          std::thread([gate_promise, verification_ticket]() {
            const auto status = display_helper_integration::wait_for_apply_verification(
              verification_ticket,
              display_helper_integration::kStreamStartApplyVerificationTimeout);
            rtsp_stream::launch_session_t::display_helper_gate_status_e gate_status =
              rtsp_stream::launch_session_t::display_helper_gate_status_e::proceed_gaveup;
            if (status == display_helper_integration::ApplyVerificationStatus::Verified) {
              gate_status = rtsp_stream::launch_session_t::display_helper_gate_status_e::proceed;
            } else if (status == display_helper_integration::ApplyVerificationStatus::Failed) {
              gate_status = rtsp_stream::launch_session_t::display_helper_gate_status_e::abort_failed;
            }
            try {
              gate_promise->set_value(gate_status);
            } catch (...) {
              // best-effort: ignore double-satisfaction
            }
          }).detach();
        }
      }

        // Apply a per-client HDR profile to physical displays (virtual displays are handled at creation time).
        const auto physical_hdr_profile_policy = display_helper_integration::request_policy::evaluate({
          .virtual_display = launch_session->virtual_display,
          .virtual_display_failed = launch_session->virtual_display_failed,
          .hdr_profile_selected = launch_session->hdr_profile && !launch_session->hdr_profile->empty(),
        });
        if (physical_hdr_profile_policy.apply_hdr_profile_to_physical) {
          const auto active_output = config::get_active_output_name();
          VDISPLAY::applyHdrProfileToOutput(
            launch_session->client_name.c_str(),
            launch_session->hdr_profile ? launch_session->hdr_profile->c_str() : nullptr,
            active_output.empty() ? nullptr : active_output.c_str()
          );
        }
#else
      display_helper_integration::DisplayApplyBuilder noop_builder;
      noop_builder.set_session(*launch_session);
      if (!display_helper_integration::apply(noop_builder.build())) {
        BOOST_LOG(warning) << "Display helper: failed to apply display configuration; continuing with existing display.";
      }
#endif


        // Probe encoders again before streaming to ensure our chosen
        // encoder matches the active GPU (which could have changed
        // due to hotplugging, driver crash, primary monitor change,
        // or any number of other factors).

#ifdef _WIN32
      bool encoder_probe_failed = false;
      bool probe_display_unavailable = false;
      if (!video::has_successful_encoder_probe()) {
        {
          VDISPLAY::ensure_display_result ensure_result {};
          auto cleanup_probe_display = util::fail_guard([&ensure_result]() {
            VDISPLAY::cleanup_ensure_display(ensure_result);
          });
          if (!VDISPLAY::policy::should_ensure_probe_display(launch_session->virtual_display)) {
            // Let APPLY settle when possible, but capability probing remains
            // adapter-scoped and does not turn a soft display gate into a 503.
            wait_for_probe_helper_settle(launch_session, display_startup_deadline);
          } else {
            ensure_result = VDISPLAY::ensure_display();
            probe_display_unavailable = !ensure_result.ready_for_probe();
          }

          if (!probe_display_unavailable) {
            encoder_probe_failed = video::probe_encoders();
          } else {
            encoder_probe_failed = true;
          }
        }
      } else {
        BOOST_LOG(debug) << "Launch encoder probe skipped (matching selected-GPU cache).";
      }
#else
      bool encoder_probe_failed = video::probe_encoders();
#endif

      if (encoder_probe_failed && !is_input_only) {
        const std::string status_message =
#ifdef _WIN32
          probe_display_unavailable ?
            "No usable display is available on the selected capture adapter." :
#endif
            "Failed to initialize video capture/encoding. Is a display connected and turned on?";
        BOOST_LOG(error) << status_message;
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", status_message);
        tree.put("root.gamesession", 0);

        return;
      }
      }

      no_active_sessions = !has_stream_session_activity();

#ifdef _WIN32
      auto pending_vulkan_hdr_layer_guard = util::fail_guard([]() {
        rtsp_stream::set_vulkan_hdr_layer_pending_stream(false);
      });
#endif

      if (is_input_only) {
        BOOST_LOG(info) << "Launching input only session..."sv;

        launch_session->client_do_cmds.clear();
        launch_session->client_undo_cmds.clear();

        // Still probe encoders once, if input only session is launched first
        // But we're ignoring if it's successful or not
        if (no_active_sessions && !proc::proc.virtual_display) {

#ifdef _WIN32
          if (has_any_active_display()) {
            video::probe_encoders();
          }
#else
        video::probe_encoders();
#endif
          // proc_t::terminate() leaves the app id at -1, so an idle host reports a
          // non-positive id rather than 0 once anything has ever run. Testing for 0
          // alone stopped input-only sessions from launching after the first app exit.
          if (current_appid <= 0) {
            proc::proc.launch_input_only();
          }
        }
      } else if (appid > 0 || !appuuid_str.empty()) {
        if (appid == current_appid || (!appuuid_str.empty() && appuuid_str == current_app_uuid)) {
          // We're basically resuming the same app

          BOOST_LOG(debug) << "Resuming app [" << proc::proc.get_last_run_app_name() << "] from launch app path...";

          if (!proc::proc.allow_client_commands || !verified_client->allow_client_commands) {
            launch_session->client_do_cmds.clear();
            launch_session->client_undo_cmds.clear();
          }

          if (current_appid == proc::input_only_app_id) {
            launch_session->input_only = true;
          }

        } else {
          // Resolve once at the request boundary, then carry that canonical app
          // through launch. An old artwork-versioned ID is an alias, so looking
          // it up again by the raw request value would reject valid legacy clients.
          if (!requested_app) {
            BOOST_LOG(error) << "Couldn't find app with ID ["sv << appid_str << "] or UUID ["sv << appuuid_str << ']';
            tree.put("root.<xmlattr>.status_code", 404);
            tree.put("root.<xmlattr>.status_message", "Cannot find requested application");
            tree.put("root.gamesession", 0);
            return;
          }

          if (!requested_app->allow_client_commands || !verified_client->allow_client_commands) {
            launch_session->client_do_cmds.clear();
            launch_session->client_undo_cmds.clear();
          }

#ifdef _WIN32
          rtsp_stream::set_vulkan_hdr_layer_pending_stream(rtsp_stream::effective_hdr_requested(*launch_session));
#endif
          auto err = proc::proc.execute(*requested_app, launch_session);
          if (err) {
            tree.put("root.<xmlattr>.status_code", err);
            tree.put(
              "root.<xmlattr>.status_message",
              err == 503 ? "Failed to initialize video capture/encoding. Is a display connected and turned on?" : "Failed to start the specified application"
            );
            tree.put("root.gamesession", 0);

            return;
          }
        }
      } else {
        tree.put("root.<xmlattr>.status_code", 403);
        tree.put("root.<xmlattr>.status_message", "How did you get here?");
        tree.put("root.gamesession", 0);
      }

      tree.put("root.<xmlattr>.status_code", 200);
      tree.put(
        "root.sessionUrl0",
        std::format(
          "{}{}:{}",
          launch_session->rtsp_url_scheme,
          net::addr_to_url_escaped_string(request->local_endpoint().address()),
          static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
        )
      );
      keep_runtime_overrides = true;
      tree.put("root.gamesession", 1);
#ifdef _WIN32
      tree.put("root.VirtualDisplayDriverReady", proc::vDisplayDriverStatus.load(std::memory_order_acquire) == VDISPLAY::DRIVER_STATUS::OK);
#else
      tree.put("root.VirtualDisplayDriverReady", false);
#endif
#ifdef _WIN32
      pending_vulkan_hdr_layer_guard.disable();
#endif

      stream::session::arm_shared_runtime_cleanup(
        launch_session->virtual_display_guid_bytes
      );
      rtsp_stream::launch_session_raise(launch_session);
#ifdef _WIN32
      virtual_display_teardown_guard.disable();
#endif
      revert_display_configuration = false;
      output_override_guard.disable();
      runtime_overrides_guard.disable();
    }


  void resume(bool &host_audio, resp_https_t response, req_https_t request, int current_appid) {
    print_req<SunshineHTTPS>(request);

#ifdef _WIN32
    // See launch(): the response fail guard can restore through the helper
    // after the virtual-display teardown guard has already completed.
    stream::session::cleanup_reservation_t cleanup_reservation;
#endif
    pt::ptree tree;
    bool revert_display_configuration {false};
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      if (tree.empty()) {
        BOOST_LOG(error) << EMPTY_PROPERTY_TREE_ERROR_MSG;
      }

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;

      if (revert_display_configuration) {
        display_helper_integration::revert();
      }
    });

    auto verified_client = get_verified_cert(request);
    const auto request_client_identity = resolve_client_identity(request, verified_client);
    if (!has_client_perm(verified_client, PERM::_allow_view)) {
      log_permission_denied("ViewApp"sv, "View stream"sv, verified_client);

      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", permission_denied_status_message(verified_client, "View stream"sv));

      return;
    }

    // proc_t::terminate() leaves the app id at -1 and nothing resets it to 0, so any
    // non-positive id means nothing is running. Comparing against 0 alone would let a
    // stale /resume run the whole resume path for an app that no longer exists.
    if (current_appid <= 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "No running app to resume");

      return;
    }

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required resume parameter");

      return;
    }

    // Newer Moonlight clients send localAudioPlayMode on /resume too,
    // so we should use it if it's present in the args and there are
    // no active sessions we could be interfering with.

    // A pending launch must not reject this request: /resume is how a second viewer joins
    // an app another client already started, and that client's launch stays pending for
    // seconds (display apply, verification, encoder probe). has_stream_session_activity()
    // already counts pending launches, so every mutating decision below degrades to a
    // plain join on its own.
    const bool no_active_sessions = !has_stream_session_activity();
    std::unordered_map<std::string, std::string> requested_runtime_overrides;
    if (auto running_app = proc::proc.resolve_app(current_appid)) {
      config::merge_config_overrides(requested_runtime_overrides, running_app->config_overrides);
    }

    auto client_settings = verified_client;
    std::string client_uuid = request_client_identity.uuid;
    const auto resume_client_uuid = resolve_known_client_uuid_from_launch_id(get_arg(args, "uniqueid", ""));
    if (client_uuid.empty()) {
      client_uuid = resume_client_uuid;
    } else if (!resume_client_uuid.empty() && is_placeholder_client_name(request_client_identity.name)) {
      BOOST_LOG(warning) << "Ignoring placeholder TLS client identity '" << request_client_identity.name
                         << "' for runtime overrides; using resume uniqueid " << resume_client_uuid << ".";
      client_uuid = resume_client_uuid;
      client_settings.reset();
    }
    if (!client_settings && !client_uuid.empty()) {
      client_settings = get_client_snapshot_by_uuid(client_uuid);
    }
    if (client_settings) {
      config::merge_config_overrides(requested_runtime_overrides, client_settings->config_overrides);
    }

#ifdef _WIN32
    if (client_settings &&
        !client_settings->hdr_profile.empty() &&
        !requested_runtime_overrides.contains("rtx_hdr_peak_brightness")) {
      if (const auto profile_peak = VDISPLAY::hdr_profile_peak_luminance_nits(client_settings->hdr_profile)) {
        const auto effective_peak = std::clamp<std::uint32_t>(*profile_peak, 400, 2000);
        requested_runtime_overrides.insert_or_assign("rtx_hdr_peak_brightness", std::to_string(effective_peak));
      }
    }
#endif

    if (!no_active_sessions &&
        !config::adapter_config_overrides_compatible_with_active(requested_runtime_overrides)) {
      BOOST_LOG(warning) << "Rejecting shared resume with a capture adapter selection that differs from the active stream.";
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put(
        "root.<xmlattr>.status_message",
        "Another stream is active with a different capture adapter selection"
      );
      return;
    }

    bool runtime_overrides_reapplied = false;
    auto previous_runtime_overrides = config::runtime_config_overrides_snapshot();
    auto runtime_overrides_guard = util::fail_guard([&]() {
      if (!runtime_overrides_reapplied) {
        return;
      }
      config::set_runtime_config_overrides(std::move(previous_runtime_overrides));
      if (!has_stream_session_activity()) {
        config::apply_config_now();
      } else {
        config::mark_deferred_reload();
      }
    });

    if (no_active_sessions) {
      config::set_runtime_config_overrides(std::move(requested_runtime_overrides));
      config::apply_config_now();
      runtime_overrides_reapplied = true;
    }

    const bool is_input_only = config::input.enable_input_only_mode && current_appid == proc::input_only_app_id;
    const bool allow_display_changes = config::video.dd.config_revert_on_disconnect && !is_input_only;
    if (no_active_sessions && allow_display_changes) {
      config::set_runtime_output_name_override(std::nullopt);
    }
    if (no_active_sessions && args.find("localAudioPlayMode"s) != std::end(args)) {
      host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    }
#ifdef _WIN32
    if (no_active_sessions) {
      stream::cancel_paused_display_cleanup();
    }
#endif
    // Prevent interleaving with hot-apply while we prep/resume a session
    auto _hot_apply_gate = config::acquire_apply_read_gate();
    if (no_active_sessions) {
      config::record_active_adapter_config();
    }

    auto launch_session = make_launch_session_from_snapshot(host_audio, is_input_only, args, verified_client, &request_client_identity);
    if (!proc::proc.allow_client_commands || !verified_client->allow_client_commands) {
      launch_session->client_do_cmds.clear();
      launch_session->client_undo_cmds.clear();
    }

    // A resume request carries no appid or appuuid, so make_launch_session() cannot resolve the
    // app and leaves every per-app setting at its default. Seed them from the app that is still
    // running, before the display is prepared below. Without this the session silently falls
    // back to the global output setting on reconnect and ignores the app's display policy.
    if (launch_session->appid <= 0 && current_appid > 0) {
      try {
        if (auto app_ctx = proc::proc.resolve_app(current_appid)) {
          launch_session->appid = (int) util::from_view(app_ctx->id);
          launch_session->gen1_framegen_fix = app_ctx->gen1_framegen_fix;
          launch_session->gen2_framegen_fix = app_ctx->gen2_framegen_fix;
          launch_session->frame_generation_enabled = app_ctx->frame_generation_enabled;
          launch_session->lossless_scaling_framegen = app_ctx->lossless_scaling_framegen;
          launch_session->lossless_scaling_target_fps = app_ctx->lossless_scaling_target_fps;
          launch_session->lossless_scaling_rtss_limit = app_ctx->lossless_scaling_rtss_limit;
          launch_session->frame_generation_provider = app_ctx->frame_generation_provider;
          rtsp_stream::launch_session_t::app_metadata_t metadata;
          metadata.id = app_ctx->id;
          metadata.name = app_ctx->name;
          metadata.virtual_screen = app_ctx->virtual_screen || app_ctx->virtual_display;
          metadata.has_command = !app_ctx->cmd.empty();
          metadata.has_playnite = !app_ctx->playnite_id.empty();
          metadata.playnite_fullscreen = app_ctx->playnite_fullscreen;
          // launch_session->virtual_display is deliberately left alone: the launch path assigns
          // it from the app and then immediately overwrites it with the request's virtualDisplay
          // argument and the per-client setting, both of which are already resolved here.
          if (!launch_session->virtual_display_mode_override && app_ctx->virtual_display_mode_override) {
            launch_session->virtual_display_mode_override = app_ctx->virtual_display_mode_override;
          }
          if (!launch_session->virtual_display_layout_override && app_ctx->virtual_display_layout_override) {
            launch_session->virtual_display_layout_override = app_ctx->virtual_display_layout_override;
          }
          if (!launch_session->dd_config_option_override && app_ctx->dd_config_option_override) {
            launch_session->dd_config_option_override = app_ctx->dd_config_option_override;
          }
          if (app_ctx->output_name_override) {
            launch_session->output_name_override = *app_ctx->output_name_override;
          }
          launch_session->app_metadata = std::move(metadata);
          BOOST_LOG(debug) << "Resume request carried no app id; applying settings from running app ["sv
                           << app_ctx->name << "] (id " << app_ctx->id << ")."sv;
        }
      } catch (...) {
      }
    }

#ifdef _WIN32
    const auto display_startup_deadline =
      std::chrono::steady_clock::now() +
      display_helper_integration::kStreamStartApplyVerificationTimeout;
    const auto display_startup_cancelled = [display_startup_deadline] {
      return std::chrono::steady_clock::now() >= display_startup_deadline;
    };
    if (allow_display_changes) {
      // Stop any in-flight helper restore loop before resuming display changes.
      (void) display_helper_integration::disarm_pending_restore(
        display_startup_cancelled,
        display_startup_deadline
      );
    }
#endif
    std::optional<std::string> pending_output_override;
    auto output_override_guard = util::fail_guard([&]() {
      if (pending_output_override) {
        config::set_runtime_output_name_override(std::nullopt);
      }
    });

#ifdef _WIN32
    std::optional<video::encoder_probe_adapter_hint_lease_t> pending_adapter_hint;
    auto pending_adapter_hint_guard = util::fail_guard([&] {
      if (pending_adapter_hint) {
        (void) video::clear_pending_virtual_display_adapter_hint(*pending_adapter_hint);
      }
    });
    prepare_virtual_display_for_session(
      launch_session,
      no_active_sessions,
      allow_display_changes,
      is_input_only,
      pending_output_override,
      pending_adapter_hint,
      display_startup_cancelled,
      display_startup_deadline
    );

    auto virtual_display_teardown_guard = util::fail_guard([&]() {
      stream::session::cleanup_reservation_t cleanup_reservation;
      if (has_stream_session_activity()) {
        return;
      }

      if (!launch_session->virtual_display) {
        return;
      }

      BOOST_LOG(info) << "Resume aborted before session start; removing virtual displays.";
      (void) platf::virtual_display_cleanup::run(
        "resume_aborted",
        config::video.dd.config_revert_on_disconnect
      );
    });
#endif

    if (no_active_sessions) {
      // We want to prepare display only if there are no active sessions at
      // the moment. This should be done before probing encoders as it could
      // change the active displays.
      const bool should_apply_display_request =
        allow_display_changes ||
        launch_session->virtual_display_recreated_on_demand ||
        launch_session->virtual_display_needs_resume_apply;
      if (should_apply_display_request) {
        BOOST_LOG(debug) << "Display helper: applying session display request on "
                         << (allow_display_changes ? "normal start/resume" :
                                                       (launch_session->virtual_display_recreated_on_demand ?
                                                          "resume virtual-display recreation" :
                                                          "resume virtual-display refresh"))
                         << " for client '" << launch_session->client_name << "'.";
        revert_display_configuration = allow_display_changes || launch_session->virtual_display_failed;

#ifdef _WIN32
        const bool helper_session_available = display_helper_session_available();
        (void) display_helper_integration::disarm_pending_restore(
          display_startup_cancelled,
          display_startup_deadline
        );
        auto request = display_helper_integration::helpers::build_request_from_session(config::video, *launch_session);
        if (!request) {
          BOOST_LOG(warning) << "Display helper: failed to build display configuration request; continuing with existing display.";
        }

        if (request) {
          display_helper_integration::ApplyVerificationTicket verification_ticket;
          const bool applied = display_helper_integration::apply(
            *request,
            &verification_ticket,
            display_startup_cancelled,
            display_helper_integration::ApplyRetryPolicy::StreamStart,
            display_startup_deadline);
          if (!applied) {
            if (helper_session_available) {
              BOOST_LOG(warning) << "Display helper: failed to apply display configuration; continuing with existing display.";
            }
          } else {
            auto gate_promise = std::make_shared<std::promise<rtsp_stream::launch_session_t::display_helper_gate_status_e>>();
            launch_session->display_helper_gate = gate_promise->get_future().share();
            BOOST_LOG(debug) << "Display helper: gating capture start on helper verification (non-blocking session resume).";
            std::thread([gate_promise, verification_ticket]() {
              const auto status = display_helper_integration::wait_for_apply_verification(
                verification_ticket,
                display_helper_integration::kStreamStartApplyVerificationTimeout);
              rtsp_stream::launch_session_t::display_helper_gate_status_e gate_status =
                rtsp_stream::launch_session_t::display_helper_gate_status_e::proceed_gaveup;
              if (status == display_helper_integration::ApplyVerificationStatus::Verified) {
                gate_status = rtsp_stream::launch_session_t::display_helper_gate_status_e::proceed;
              } else if (status == display_helper_integration::ApplyVerificationStatus::Failed) {
                gate_status = rtsp_stream::launch_session_t::display_helper_gate_status_e::abort_failed;
              }
              try {
                gate_promise->set_value(gate_status);
              } catch (...) {
                // best-effort: ignore double-satisfaction
              }
            }).detach();
          }
        }

        // Apply a per-client HDR profile to physical displays (virtual displays are handled at creation time).
        const auto physical_hdr_profile_policy = display_helper_integration::request_policy::evaluate({
          .virtual_display = launch_session->virtual_display,
          .virtual_display_failed = launch_session->virtual_display_failed,
          .hdr_profile_selected = launch_session->hdr_profile && !launch_session->hdr_profile->empty(),
        });
        if (physical_hdr_profile_policy.apply_hdr_profile_to_physical) {
          const auto active_output = config::get_active_output_name();
          VDISPLAY::applyHdrProfileToOutput(
            launch_session->client_name.c_str(),
            launch_session->hdr_profile ? launch_session->hdr_profile->c_str() : nullptr,
            active_output.empty() ? nullptr : active_output.c_str()
          );
        }
#else
      display_helper_integration::DisplayApplyBuilder noop_builder;
      noop_builder.set_session(*launch_session);
      if (!display_helper_integration::apply(noop_builder.build())) {
        BOOST_LOG(warning) << "Display helper: failed to apply display configuration; continuing with existing display.";
      }
#endif
      }

      else {
#ifdef _WIN32
        BOOST_LOG(debug) << "Display helper: skipping resume apply; only deferrals are allowed.";
#else
        display_helper_integration::DisplayApplyBuilder noop_builder;
        noop_builder.set_session(*launch_session);
        if (!display_helper_integration::apply(noop_builder.build())) {
          BOOST_LOG(warning) << "Display helper: failed to apply display configuration; continuing with existing display.";
        }
#endif
      }

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
#ifdef _WIN32
      bool encoder_probe_failed = false;
      bool probe_display_unavailable = false;
      if (!video::has_successful_encoder_probe()) {
        {
          VDISPLAY::ensure_display_result ensure_result {};
          auto cleanup_probe_display = util::fail_guard([&ensure_result]() {
            VDISPLAY::cleanup_ensure_display(ensure_result);
          });
          if (!VDISPLAY::policy::should_ensure_probe_display(launch_session->virtual_display)) {
            wait_for_probe_helper_settle(launch_session, display_startup_deadline);
          } else {
            ensure_result = VDISPLAY::ensure_display();
            probe_display_unavailable = !ensure_result.ready_for_probe();
          }

          if (!probe_display_unavailable) {
            encoder_probe_failed = video::probe_encoders();
          } else {
            encoder_probe_failed = true;
          }
        }
      } else {
        BOOST_LOG(debug) << "Resume encoder probe skipped (matching selected-GPU cache).";
      }
#else
      bool encoder_probe_failed = video::probe_encoders();
#endif

      if (encoder_probe_failed && !launch_session->input_only) {
        const std::string status_message =
#ifdef _WIN32
          probe_display_unavailable ?
            "No usable display is available on the selected capture adapter." :
#endif
            "Failed to initialize video capture/encoding. Is a display connected and turned on?";
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", status_message);

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.resume", 1);

#ifdef _WIN32

    tree.put("root.VirtualDisplayDriverReady", proc::vDisplayDriverStatus.load(std::memory_order_acquire) == VDISPLAY::DRIVER_STATUS::OK);
#else
    tree.put("root.VirtualDisplayDriverReady", false);
#endif

    stream::session::arm_shared_runtime_cleanup(
      launch_session->virtual_display_guid_bytes
    );
    rtsp_stream::launch_session_raise(launch_session);
#ifdef _WIN32
    virtual_display_teardown_guard.disable();
#endif
    output_override_guard.disable();
    runtime_overrides_guard.disable();
    revert_display_configuration = false;

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    system_tray::update_tray_client_connected(verified_client->name);
#endif
  }

  void cancel(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

#ifdef _WIN32
    // Reserve cleanup before sampling process/session state and retain it
    // through session teardown, app termination, and final display cleanup.
    stream::session::cleanup_reservation_t cleanup_reservation;
#endif

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto verified_client = get_verified_cert(request);
    if (!has_client_perm(verified_client, PERM::launch)) {
      log_permission_denied("CancelApp"sv, "Launch applications"sv, verified_client);

      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", permission_denied_status_message(verified_client, "Launch applications"sv));

      return;
    }

    tree.put("root.cancel", 1);
    tree.put("root.<xmlattr>.status_code", 200);

    const bool has_running_app = proc::proc.running() > 0;
#ifdef _WIN32
    const bool preserve_deferred_launch =
      has_running_app &&
      proc::proc.is_launch_deferred() &&
      rtsp_stream::session_count_no_cleanup() == 0;
    if (preserve_deferred_launch) {
      BOOST_LOG(info) << "Cancel requested while app launch is deferred; preserving deferred app and virtual display state.";
    }
#else
    constexpr bool preserve_deferred_launch = false;
#endif
    rtsp_stream::terminate_sessions(preserve_deferred_launch);

    if (has_running_app && !preserve_deferred_launch) {
      proc::proc.terminate();
    }
    // The config needs to be reverted regardless of whether "proc::proc.terminate()" was called or not.

#ifdef _WIN32

    // RTSP session termination above is synchronous, so by the time we reach
    // this point the old session threads have already completed their joins.
    cleanup_virtual_display_if_idle();

#endif
  }

  void appasset(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    auto fg = util::fail_guard([&]() {
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error);
      response->close_connection_after_response = true;
    });

    auto verified_client = get_verified_cert(request);

    if (!has_client_perm(verified_client, PERM::_all_actions)) {
      log_permission_denied("Get AppAsset"sv, "List applications"sv, verified_client);

      fg.disable();
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    auto args = request->parse_query_string();
    const auto appid = get_arg(args, "appid", "0");
    const auto appuuid = get_arg(args, "appuuid", "");
    auto app_ctx = proc::proc.resolve_app(appid, appuuid);
    auto app_image = app_ctx ? proc::validate_app_image_path(app_ctx->image_path) : proc::proc.get_app_image((int) util::from_view(appid));

    fg.disable();

    std::ifstream in(app_image, std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    response->close_connection_after_response = true;
  }

  void getClipboard(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    auto verified_client = get_verified_cert(request);

    if (
      !has_client_perm(verified_client, PERM::_allow_view) || !has_client_perm(verified_client, PERM::clipboard_read)
    ) {
      log_permission_denied("Read Clipboard"sv, "Read clipboard"sv, verified_client);

      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    auto args = request->parse_query_string();
    auto clipboard_type = get_arg(args, "type");
    if (clipboard_type != "text"sv) {
      BOOST_LOG(debug) << "Clipboard type [" << clipboard_type << "] is not supported!";

      response->write(SimpleWeb::StatusCode::client_error_bad_request);
      response->close_connection_after_response = true;
      return;
    }

    std::list<std::string> connected_uuids = rtsp_stream::get_all_session_uuids();

    bool found = !connected_uuids.empty();

    if (found) {
      found = (std::find(connected_uuids.begin(), connected_uuids.end(), verified_client->uuid) != connected_uuids.end());
    }

    if (!found) {
      BOOST_LOG(debug) << "Client [" << verified_client->name << "] trying to get clipboard is not connected to a stream";

      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      response->close_connection_after_response = true;
      return;
    }

    std::string content = platf::get_clipboard();
    response->write(content);
    return;
  }

  void setClipboard(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    auto verified_client = get_verified_cert(request);

    if (
      !has_client_perm(verified_client, PERM::_allow_view) || !has_client_perm(verified_client, PERM::clipboard_set)
    ) {
      log_permission_denied("Write Clipboard"sv, "Set clipboard"sv, verified_client);

      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    auto args = request->parse_query_string();
    auto clipboard_type = get_arg(args, "type");
    if (clipboard_type != "text"sv) {
      BOOST_LOG(debug) << "Clipboard type [" << clipboard_type << "] is not supported!";

      response->write(SimpleWeb::StatusCode::client_error_bad_request);
      response->close_connection_after_response = true;
      return;
    }

    std::list<std::string> connected_uuids = rtsp_stream::get_all_session_uuids();

    bool found = !connected_uuids.empty();

    if (found) {
      found = (std::find(connected_uuids.begin(), connected_uuids.end(), verified_client->uuid) != connected_uuids.end());
    }

    if (!found) {
      BOOST_LOG(debug) << "Client [" << verified_client->name << "] trying to set clipboard is not connected to a stream";

      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      response->close_connection_after_response = true;
      return;
    }

    std::string content = request->content.string();

    bool success = platf::set_clipboard(content);

    if (!success) {
      BOOST_LOG(debug) << "Setting clipboard failed!";

      response->write(SimpleWeb::StatusCode::server_error_internal_server_error);
      response->close_connection_after_response = true;
    }

    response->write();
    return;
  }

  void setBitrate(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto verified_client = get_verified_cert(request);
    if (!has_client_perm(verified_client, PERM::_allow_view)) {
      log_permission_denied("SetBitrate"sv, "View stream"sv, verified_client);
      tree.put("root.bitrate", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", permission_denied_status_message(verified_client, "View stream"sv));
      return;
    }

    auto args = request->parse_query_string();
    const int requested = (int) util::from_view(get_arg(args, "bitrate", "0"));
    if (requested <= 0) {
      tree.put("root.bitrate", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing or invalid bitrate parameter");
      return;
    }

    // Clamp to the host bitrate ceiling. max_bitrate == 0 means "unlimited" in config, so also
    // enforce an absolute ceiling to keep the value sane and avoid overflow downstream
    // (bitrate_kbps * 1000 must fit the encoder's 32-bit rate-control fields).
    constexpr int absolute_max_bitrate_kbps = 500000;  // 500 Mbps
    int applied = requested;
    if (config::video.max_bitrate > 0 && applied > config::video.max_bitrate) {
      applied = config::video.max_bitrate;
    }
    if (applied > absolute_max_bitrate_kbps) {
      applied = absolute_max_bitrate_kbps;
    }
    if (applied != requested) {
      BOOST_LOG(info) << "Clamped requested bitrate "sv << requested << " kbps to "sv << applied << " kbps"sv;
    }

    const int updated = stream::set_bitrate_for_sessions(verified_client->uuid, applied);
    if (updated <= 0) {
      BOOST_LOG(warning) << "Bitrate change requested by ["sv << verified_client->name << "] but no matching active session was found"sv;
      tree.put("root.bitrate", 0);
      tree.put("root.<xmlattr>.status_code", 404);
      tree.put("root.<xmlattr>.status_message", "No active session for this client");
      return;
    }

    BOOST_LOG(info) << "Client ["sv << verified_client->name << "] set runtime bitrate to "sv << applied << " kbps ("sv << updated << " session(s))"sv;
    tree.put("root.bitrate", applied);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void getAbrCapabilities(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    auto verified_client = get_verified_cert(request);
    if (!has_client_perm(verified_client, PERM::_allow_view)) {
      log_permission_denied("AbrCapabilities"sv, "View stream"sv, verified_client, true);
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    // Server-side adaptive bitrate decisioning is not implemented. Reporting it unsupported makes
    // Foundation-compatible clients (e.g. Moonlight V+) drive their own local ABR controller, which
    // applies decisions through the runtime /bitrate endpoint above.
    const std::string body = R"({"supported":false,"version":1,"features":["runtime_bitrate"]})";
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    response->write(SimpleWeb::StatusCode::success_ok, body, headers);
    response->close_connection_after_response = true;
  }

  void setup(const std::string &pkey, const std::string &cert) {
    conf_intern.pkey = pkey;
    conf_intern.servercert = cert;
  }

  void start() {
    platf::set_thread_name("nvhttp");
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    auto port_http = net::map_port(PORT_HTTP);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    bool clean_slate = config::sunshine.flags[config::flag::FRESH_STATE];

    if (!clean_slate) {
      load_state();
    }

    auto pkey = file_handler::read_file(config::nvhttp.pkey.c_str());
    auto cert = file_handler::read_file(config::nvhttp.cert.c_str());
    setup(pkey, cert);

    // resume doesn't always get the parameter "localAudioPlayMode"
    // launch will store it in host_audio
    bool host_audio {};

    https_server_t https_server {config::nvhttp.cert, config::nvhttp.pkey};
    http_server_t http_server;
    thread_pool_util::ThreadPool blocking_route_pool;
    blocking_route_pool.start(1);
    // Discovery routes are observation-only, so they must not queue behind the mutating
    // routes. A launch/resume/cancel handler can hold the lifecycle gate across unbounded
    // work, and on a single FIFO worker that made the host undiscoverable until restart.
    thread_pool_util::ThreadPool discovery_route_pool;
    discovery_route_pool.start(1);

    // Verify certificates after establishing connection
    https_server.verify = [](req_https_t req, SSL *ssl) {
      tl_peer_certificate.reset();
      forget_tls_client_identity(req);

      crypto::x509_t x509_verify {
#if OPENSSL_VERSION_MAJOR >= 3
        SSL_get1_peer_certificate(ssl)
#else
      SSL_get_peer_certificate(ssl)
#endif
      };

      if (!x509_verify) {
        BOOST_LOG(info) << "unknown -- denied"sv;
        return false;
      }

      bool verified = false;
      p_named_cert_t named_cert_p;

      auto fg = util::fail_guard([&]() {
        const auto subject_name = cert_subject_name_for_log(x509_verify);
        BOOST_LOG(verbose) << subject_name << " -- "sv << (verified ? "verified"sv : "denied"sv);
      });

      const char *err_str = nullptr;
      {
        std::lock_guard<std::mutex> lock(client_mutex);

        if (pending_cert_queue) {
          while (pending_cert_queue->peek()) {
            auto cert = pending_cert_queue->pop();
            if (!cert) {
              continue;
            }

            const auto subject_name = cert_subject_name_for_log(cert);
            BOOST_LOG(verbose) << "Added cert ["sv << subject_name << ']';

            const auto pem = crypto::pem(cert);
            auto named_it = std::find_if(
              client_root.named_devices.begin(),
              client_root.named_devices.end(),
              [&pem](const p_named_cert_t &named_cert) {
                return named_cert && named_cert->cert == pem;
              }
            );

            if (named_it != client_root.named_devices.end()) {
              cert_chain.add(*named_it);
            } else {
              BOOST_LOG(warning) << "Pending certificate not found in client registry: "sv << subject_name;
            }
          }
        }

        err_str = cert_chain.verify(x509_verify.get(), named_cert_p);

      }
      if (err_str) {
        BOOST_LOG(warning) << "SSL Verification error :: "sv << err_str;
        return verified;
      }

      verified = true;
      if (x509_verify) {
        if (auto identity = resolve_client_identity_from_peer_cert(x509_verify)) {
          remember_tls_client_identity(req, *identity);
        }
        tl_peer_certificate = std::move(x509_verify);
      }

      return true;
    };

    https_server.on_verify_failed = [](resp_https_t resp, req_https_t req) {
      pt::ptree tree;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_xml(data, tree);
        resp->write(data.str());
        resp->close_connection_after_response = true;
      });

      tree.put("root.<xmlattr>.status_code"s, 401);
      tree.put("root.<xmlattr>.query"s, req->path);
      tree.put("root.<xmlattr>.status_message"s, "The client is not authorized. Certificate verification failed."s);
    };

    auto run_on_blocking_pool = [](thread_pool_util::ThreadPool &pool, auto task) {
      pool.push([task = std::move(task)]() mutable {
        try {
          task();
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "Blocking NVHTTP handler failed: " << e.what();
        } catch (...) {
          BOOST_LOG(error) << "Blocking NVHTTP handler failed with an unknown exception";
        }
      });
    };

    auto run_blocking_nvhttp = [&blocking_route_pool, run_on_blocking_pool](auto task) {
      run_on_blocking_pool(blocking_route_pool, std::move(task));
    };

    auto run_discovery_nvhttp = [&discovery_route_pool, run_on_blocking_pool](auto task) {
      run_on_blocking_pool(discovery_route_pool, std::move(task));
    };

    https_server.default_resource["GET"] = not_found<SunshineHTTPS>;
    https_server.default_resource["POST"] = not_found<SunshineHTTPS>;
    https_server.resource["^/serverinfo$"]["GET"] = [run_discovery_nvhttp](auto resp, auto req) {
      run_discovery_nvhttp([resp = std::move(resp), req = std::move(req)]() mutable {
        serverinfo<SunshineHTTPS>(std::move(resp), std::move(req));
      });
    };
    https_server.resource["^/pair/?$"]["GET"] = pair<SunshineHTTPS>;
    https_server.resource["^/pair/?$"]["POST"] = pair<SunshineHTTPS>;
    https_server.resource["^/unpair/?$"]["GET"] = unpair<SunshineHTTPS>;
    https_server.resource["^/unpair/?$"]["POST"] = unpair<SunshineHTTPS>;
    https_server.resource["^/applist$"]["GET"] = [run_discovery_nvhttp](auto resp, auto req) {
      run_discovery_nvhttp([resp = std::move(resp), req = std::move(req)]() mutable {
        applist(std::move(resp), std::move(req));
      });
    };
    https_server.resource["^/appasset$"]["GET"] = appasset;
    https_server.resource["^/launch$"]["GET"] = [&host_audio, run_blocking_nvhttp](auto resp, auto req) {
      run_blocking_nvhttp([&host_audio, resp = std::move(resp), req = std::move(req)]() mutable {
        std::lock_guard launch_lock {launch_request_mutex};
        (void) proc::proc.running();
        std::lock_guard lifecycle_lock {stream_lifecycle_gate};
        const int current_appid = proc::proc.current_app_id();
        launch(host_audio, std::move(resp), std::move(req), current_appid);
      });
    };
    https_server.resource["^/resume$"]["GET"] = [&host_audio, run_blocking_nvhttp](auto resp, auto req) {
      run_blocking_nvhttp([&host_audio, resp = std::move(resp), req = std::move(req)]() mutable {
        std::lock_guard launch_lock {launch_request_mutex};
        (void) proc::proc.running();
        std::lock_guard lifecycle_lock {stream_lifecycle_gate};
        const int current_appid = proc::proc.current_app_id();
        resume(host_audio, std::move(resp), std::move(req), current_appid);
      });
    };
    https_server.resource["^/cancel$"]["GET"] = [run_blocking_nvhttp](auto resp, auto req) {
      run_blocking_nvhttp([resp = std::move(resp), req = std::move(req)]() mutable {
        std::lock_guard lock {launch_request_mutex};
        cancel(std::move(resp), std::move(req));
      });
    };
    https_server.resource["^/actions/clipboard$"]["GET"] = getClipboard;
    https_server.resource["^/actions/clipboard$"]["POST"] = setClipboard;
    https_server.resource["^/bitrate$"]["GET"] = setBitrate;
    https_server.resource["^/api/abr/capabilities$"]["GET"] = getAbrCapabilities;

    https_server.config.reuse_address = true;
    https_server.config.address = net::get_bind_address(address_family);
    https_server.config.port = port_https;

    http_server.default_resource["GET"] = not_found<SimpleWeb::HTTP>;
    http_server.default_resource["POST"] = not_found<SimpleWeb::HTTP>;
    http_server.resource["^/serverinfo$"]["GET"] = [run_discovery_nvhttp](auto resp, auto req) {
      run_discovery_nvhttp([resp = std::move(resp), req = std::move(req)]() mutable {
        serverinfo<SimpleWeb::HTTP>(std::move(resp), std::move(req));
      });
    };
    http_server.resource["^/pair/?$"]["GET"] = pair<SimpleWeb::HTTP>;
    http_server.resource["^/pair/?$"]["POST"] = pair<SimpleWeb::HTTP>;
    http_server.resource["^/unpair/?$"]["GET"] = unpair<SimpleWeb::HTTP>;
    http_server.resource["^/unpair/?$"]["POST"] = unpair<SimpleWeb::HTTP>;

    http_server.config.reuse_address = true;
    http_server.config.address = net::get_bind_address(address_family);
    http_server.config.port = port_http;

    auto accept_and_run = [&](auto *http_server) {
      try {
        std::string name = "nvhttp::" + std::to_string(http_server->config.port);
        platf::set_thread_name(name);
        http_server->start();
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling http_server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }

        BOOST_LOG(fatal) << "Couldn't start http server on ports ["sv << port_https << ", "sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::thread ssl {accept_and_run, &https_server};
    std::thread tcp {accept_and_run, &http_server};

    // Wait for any event
    shutdown_event->view();

    map_id_sess.clear();

    https_server.stop();
    http_server.stop();

    ssl.join();
    tcp.join();
    blocking_route_pool.stop();
    blocking_route_pool.join();
    discovery_route_pool.stop();
    discovery_route_pool.join();
  }

  std::string request_otp(const std::string &passphrase, const std::string &deviceName) {
    if (passphrase.size() < 4) {
      return "";
    }

    one_time_pin = crypto::rand_alphabet(4, "0123456789"sv);
    otp_passphrase = passphrase;
    otp_device_name = deviceName;
    otp_creation_time = std::chrono::steady_clock::now();

    return one_time_pin;
  }

  void erase_all_clients() {
    {
      std::lock_guard<std::mutex> lock(client_mutex);
      client_root = client_t {};
      cert_chain.clear();
    }
    save_state();
    load_state();
  }

  void stop_session(stream::session_t &session, bool graceful) {
    if (graceful) {
      stream::session::graceful_stop(session);
    } else {
      stream::session::stop(session);
    }
  }

  bool find_and_stop_session(const std::string &uuid, bool graceful) {
    auto session = rtsp_stream::find_session(uuid);
    if (session) {
      stop_session(*session, graceful);
      return true;
    }
    return false;
  }

  bool disconnect_client(const std::string &uuid) {
    return rtsp_stream::disconnect_client_sessions(uuid);
  }

  bool has_client_uuid(std::string_view uuid) {
    std::lock_guard<std::mutex> lock(client_mutex);
    for (const auto &named_cert : client_root.named_devices) {
      if (named_cert->uuid == uuid) {
        return true;
      }
    }
    return false;
  }

  bool get_client_always_use_virtual_display(const std::string &uuid) {
    std::lock_guard<std::mutex> lock(client_mutex);
    for (const auto &named_cert : client_root.named_devices) {
      if (named_cert->uuid == uuid) {
        return named_cert->always_use_virtual_display;
      }
    }
    return false;
  }

  std::unordered_map<std::string, std::string> get_client_config_overrides(const std::string &uuid) {
    std::lock_guard<std::mutex> lock(client_mutex);
    for (const auto &named_cert : client_root.named_devices) {
      if (named_cert->uuid == uuid) {
        auto overrides = named_cert->config_overrides;
#ifdef _WIN32
        if (!named_cert->hdr_profile.empty() && !overrides.contains("rtx_hdr_peak_brightness")) {
          if (const auto profile_peak = VDISPLAY::hdr_profile_peak_luminance_nits(named_cert->hdr_profile)) {
            const auto effective_peak = std::clamp<std::uint32_t>(*profile_peak, 400, 2000);
            overrides.insert_or_assign("rtx_hdr_peak_brightness", std::to_string(effective_peak));
          }
        }
#endif
        return overrides;
      }
    }
    return {};
  }

  void update_session_info(stream::session_t &session, const std::string &name, const crypto::PERM newPerm) {
    stream::session::update_device_info(session, name, newPerm);
  }

  bool find_and_udpate_session_info(const std::string &uuid, const std::string &name, const crypto::PERM newPerm) {
    auto session = rtsp_stream::find_session(uuid);
    if (session) {
      update_session_info(*session, name, newPerm);
      return true;
    }
    return false;
  }

  bool update_device_info(
    const std::string &uuid,
    const std::string &name,
    const std::string &display_mode,
    const std::string &output_name_override,
    const bool always_use_virtual_display,
    const std::string &virtual_display_mode,
    const std::string &virtual_display_layout,
    std::optional<std::unordered_map<std::string, std::string>> config_overrides,
    const bool prefer_10bit_sdr,
    const std::optional<std::string> hdr_profile
  ) {
    if (uuid.empty()) {
      return false;
    }

    const auto trimmed_name = boost::algorithm::trim_copy(name);
    if (is_placeholder_client_name(trimmed_name)) {
      BOOST_LOG(warning) << "Refusing to update paired client '" << uuid << "' to reserved name '" << trimmed_name << "'.";
      return false;
    }
    const auto trimmed_display_mode = boost::algorithm::trim_copy(display_mode);
    const auto trimmed_output_override = boost::algorithm::trim_copy(output_name_override);
    const auto trimmed_vd_mode = boost::algorithm::trim_copy(virtual_display_mode);
    const auto trimmed_vd_layout = boost::algorithm::trim_copy(virtual_display_layout);
    if (config_overrides) {
      std::unordered_map<std::string, std::string> normalized_overrides;
      config::merge_config_overrides(normalized_overrides, *config_overrides);
      config_overrides = std::move(normalized_overrides);
    }

    bool updated = false;
    {
      std::lock_guard<std::mutex> lock(client_mutex);
      for (auto &named_cert : client_root.named_devices) {

        if (named_cert->uuid != uuid) {
          continue;
        }

        named_cert->name = trimmed_name;
        named_cert->display_mode = trimmed_display_mode;
        named_cert->always_use_virtual_display = always_use_virtual_display;
        named_cert->output_name_override = always_use_virtual_display ? "" : trimmed_output_override;
        named_cert->virtual_display_mode_override = trimmed_vd_mode;
        named_cert->virtual_display_layout_override = trimmed_vd_layout;
        named_cert->prefer_10bit_sdr = prefer_10bit_sdr;
        if (config_overrides) {
          named_cert->config_overrides = std::move(*config_overrides);
        }
        if (hdr_profile.has_value()) {
          named_cert->hdr_profile = boost::algorithm::trim_copy(*hdr_profile);

        }
        updated = true;
        break;
      }
    }

    if (updated) {
      save_state();
    }
    return updated;
  }

  bool set_client_hdr_profile(const std::string &uuid, const std::string &hdr_profile) {
    if (uuid.empty()) {
      return false;
    }

    const auto trimmed_hdr_profile = boost::algorithm::trim_copy(hdr_profile);

    bool updated = false;
    {
      std::lock_guard<std::mutex> lock(client_mutex);
      for (auto &named_cert : client_root.named_devices) {

        if (named_cert->uuid != uuid) {
          continue;
        }

        named_cert->hdr_profile = trimmed_hdr_profile;
        updated = true;
        break;
      }
    }

    if (updated) {
      save_state();
    }
    return updated;

  }

  bool update_device_info(
    const std::string &uuid,
    const std::string &name,
    const std::string &display_mode,
    const std::string &output_name_override,
    const cmd_list_t &do_cmds,
    const cmd_list_t &undo_cmds,
    const crypto::PERM newPerm,
    const bool enable_legacy_ordering,
    const bool allow_client_commands,
    const bool always_use_virtual_display,
    const std::string &virtual_display_mode,
    const std::string &virtual_display_layout,
    const bool prefer_10bit_sdr
  ) {
    find_and_udpate_session_info(uuid, name, newPerm);


    bool updated = false;
    {
      std::lock_guard<std::mutex> lock(client_mutex);
      for (auto &named_cert_p : client_root.named_devices) {
        if (named_cert_p->uuid == uuid) {
          named_cert_p->name = name;
          named_cert_p->display_mode = display_mode;
          named_cert_p->output_name_override = output_name_override;
          named_cert_p->perm = newPerm;
          named_cert_p->do_cmds = do_cmds;
          named_cert_p->undo_cmds = undo_cmds;
          named_cert_p->enable_legacy_ordering = enable_legacy_ordering;
          named_cert_p->allow_client_commands = allow_client_commands;
          named_cert_p->always_use_virtual_display = always_use_virtual_display;
          named_cert_p->virtual_display_mode_override = virtual_display_mode;
          named_cert_p->virtual_display_layout_override = virtual_display_layout;
          named_cert_p->prefer_10bit_sdr = prefer_10bit_sdr;
          updated = true;
          break;
        }

      }
    }

    if (updated) {
      save_state();
    }
    return updated;
  }

  // (Windows-only) display_helper_integration is included above

  bool unpair_client(const std::string_view uuid) {
    bool removed = false;

    bool empty = false;
    {
      std::lock_guard<std::mutex> lock(client_mutex);
      for (auto it = client_root.named_devices.begin(); it != client_root.named_devices.end();) {
        if ((*it)->uuid == uuid) {

          it = client_root.named_devices.erase(it);
          removed = true;
        } else {
          ++it;
        }
      }
      empty = client_root.named_devices.empty();
    }

    save_state();
    load_state();

    if (removed) {
      auto session = rtsp_stream::find_session(uuid);
      if (session) {
        stop_session(*session, true);
      }

      if (empty) {
        proc::proc.terminate();
      }
    }

    return removed;
  }
}  // namespace nvhttp
