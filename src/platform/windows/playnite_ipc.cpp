/**
 * @file src/platform/windows/playnite_ipc.cpp
 * @brief Playnite plugin IPC client using Windows named pipes with anonymous handshake.
 */

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include "playnite_ipc.h"

#ifndef SUNSHINE_PLAYNITE_LAUNCHER
  #include "src/platform/windows/misc.h"

#endif
#include "src/utility.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

using namespace std::chrono_literals;

#ifdef SUNSHINE_PLAYNITE_LAUNCHER
// The launcher never runs as SYSTEM; provide a local stub to avoid linking heavy deps.
namespace platf {
  inline bool is_running_as_system() {
    return false;
  }
}  // namespace platf
#endif

namespace platf::playnite {
  namespace {
    inline constexpr char kControlPipeName[] = "Sunshine.PlayniteExtension";
    // Reconnect backoff: start at 250 ms, double per failure, cap at 10 s.
    inline constexpr std::chrono::milliseconds kRetryInitialDelay {250};
    inline constexpr std::chrono::milliseconds kRetryMaxDelay {10000};
    // A connection must stay up this long before the backoff resets, so a
    // plugin that accepts and immediately drops us cannot cause a tight loop.
    inline constexpr std::chrono::seconds kStableConnection {5};
    inline constexpr std::chrono::seconds kFailureSummaryInterval {60};
  }  // namespace

  IpcClient::IpcClient():
      control_name_(kControlPipeName) {}

  IpcClient::IpcClient(const std::string &control_name):
      control_name_(control_name.empty() ? kControlPipeName : control_name) {}

  IpcClient::~IpcClient() {
    stop();
  }

  void IpcClient::start() {
    if (running_.exchange(true)) {
      return;
    }
    worker_ = std::thread([this]() {
      this->run();
    });
  }

  void IpcClient::stop() {
    running_.store(false);
    {
      std::scoped_lock lk(pipe_mutex_);
      if (pipe_) {
        pipe_->stop();
      }
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    {
      std::scoped_lock lk(pipe_mutex_);
      pipe_.reset();
      recv_buffer_.clear();
    }
    active_.store(false);
  }

  void IpcClient::run() {
    while (running_.load()) {
      broken_.store(false);

      if (!is_playnite_running()) {
        if (!no_playnite_logged_) {
          BOOST_LOG(debug) << "Playnite IPC: Playnite not running; deferring client connection";
          no_playnite_logged_ = true;
        }
        // IPC client is only started when there's a session or API activity,
        // so use a reasonable retry interval (not aggressive polling)
        sleep_while_running(2s);
        continue;
      }

      if (no_playnite_logged_) {
        BOOST_LOG(debug) << "Playnite IPC: Playnite detected; attempting client connection";
        no_playnite_logged_ = false;
        retry_delay_ = 0ms;
      }

      unsigned long connect_error = 0;
      auto data_pipe = connect_to_plugin(connect_error);
      if (!data_pipe) {
        note_connect_failure("could not connect to the Playnite plugin pipe", connect_error);
        wait_before_retry();
        continue;
      }

      auto on_msg = [this](std::span<const uint8_t> bytes) {
        accumulate_and_dispatch_lines(bytes);
      };
      auto on_err = [](const std::string &err) {
        BOOST_LOG(error) << "Playnite IPC: client pipe error: " << err;
      };
      auto on_broken = [this]() {
        BOOST_LOG(warning) << "Playnite IPC: client pipe broken";
        broken_.store(true);
      };

      bool pipe_started = false;
      {
        std::scoped_lock lk(pipe_mutex_);
        pipe_ = std::make_unique<platf::dxgi::AsyncNamedPipe>(std::move(data_pipe));
        pipe_started = pipe_->start(on_msg, on_err, on_broken);
        if (!pipe_started) {
          pipe_.reset();
        }
      }
      if (!pipe_started) {
        note_connect_failure("failed to start async pipe", 0);
        wait_before_retry();
        continue;
      }

      note_connect_success();
      const auto connected_at = std::chrono::steady_clock::now();
      active_.store(true);
      if (connected_handler_) {
        try {
          connected_handler_();
        } catch (...) {}
      }
      serve_connected_loop();
      {
        std::scoped_lock lk(pipe_mutex_);
        if (pipe_) {
          pipe_->stop();
          pipe_.reset();
        }
      }
      active_.store(false);
      if (disconnected_handler_) {
        try {
          disconnected_handler_();
        } catch (...) {}
      }

      if (!running_.load()) {
        break;
      }
      if (std::chrono::steady_clock::now() - connected_at >= kStableConnection) {
        retry_delay_ = 0ms;
      }
      wait_before_retry();
    }
  }

  void IpcClient::sleep_while_running(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (running_.load()) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        break;
      }
      std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(deadline - now, 100ms));
    }
  }

  void IpcClient::wait_before_retry() {
    if (retry_delay_ < kRetryInitialDelay) {
      retry_delay_ = kRetryInitialDelay;
    }
    // Jitter over the upper half of the delay keeps clients from retrying in lockstep.
    thread_local std::minstd_rand rng {std::random_device {}()};
    const auto ms = retry_delay_.count();
    std::uniform_int_distribution<long long> jitter(ms / 2, ms);
    const auto delay = std::chrono::milliseconds(jitter(rng));
    retry_delay_ = std::min(retry_delay_ * 2, kRetryMaxDelay);
    sleep_while_running(delay);
  }

  void IpcClient::note_connect_failure(const char *what, unsigned long error) {
    ++failed_attempts_;
    ++failures_since_summary_;
    const auto now = std::chrono::steady_clock::now();
    if (!failing_) {
      failing_ = true;
      failures_since_summary_ = 0;
      last_failure_summary_ = now;
      BOOST_LOG(warning) << "Playnite IPC: " << what << " (error " << error
                         << "); retrying with backoff up to "
                         << std::chrono::duration_cast<std::chrono::seconds>(kRetryMaxDelay).count() << " s";
      return;
    }
    if (now - last_failure_summary_ >= kFailureSummaryInterval) {
      BOOST_LOG(warning) << "Playnite IPC: still failing to connect, " << failures_since_summary_
                         << " attempts in last "
                         << std::chrono::duration_cast<std::chrono::seconds>(now - last_failure_summary_).count()
                         << " s (last: " << what << ", error " << error << ")";
      failures_since_summary_ = 0;
      last_failure_summary_ = now;
    }
  }

  void IpcClient::note_connect_success() {
    if (failing_) {
      BOOST_LOG(info) << "Playnite IPC: connected after " << failed_attempts_ << " failed attempt(s)";
    }
    failing_ = false;
    failed_attempts_ = 0;
    failures_since_summary_ = 0;
  }

  void IpcClient::accumulate_and_dispatch_lines(std::span<const uint8_t> bytes) {
    if (!bytes.empty()) {
      recv_buffer_.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }
    size_t start = 0;
    while (true) {
      auto pos = recv_buffer_.find('\n', start);
      if (pos == std::string::npos) {
        break;
      }
      std::string line = recv_buffer_.substr(start, pos - start);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      start = pos + 1;
      auto non_ws = line.find_first_not_of(" \t\r\n");
      if (non_ws == std::string::npos) {
        continue;
      }
      if (handler_) {
        handler_(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(line.data()), line.size()));
      }
    }
    if (start > 0) {
      recv_buffer_.erase(0, start);
    }
  }

  std::unique_ptr<platf::dxgi::INamedPipe> IpcClient::connect_to_plugin(unsigned long &error_out) {
    platf::dxgi::AnonymousPipeFactory factory;
    // run() reports repeated connect failures with its own rate limit.
    factory.set_quiet_connect_failures();
    // Playnite may identify the SYSTEM service without SeImpersonatePrivilege.
    // Apply to both control and data connections, never to unrelated helper IPC.
    factory.set_client_identification_only();
    const char *ctl = control_name_.empty() ? kControlPipeName : control_name_.c_str();
    if (!failing_) {
      BOOST_LOG(debug) << "Playnite IPC: connecting to control pipe '" << ctl << "'";
    }
    auto pipe = factory.create_client(ctl);
    if (!pipe) {
      error_out = factory.last_connect_error();
      return nullptr;
    }
    BOOST_LOG(debug) << "Playnite IPC: data pipe acquired";
    return pipe;
  }

  void IpcClient::serve_connected_loop() {
    BOOST_LOG(debug) << "Playnite IPC: client connected";
    while (running_.load() && !broken_.load()) {
      bool connected = false;
      {
        std::scoped_lock lk(pipe_mutex_);
        connected = pipe_ && pipe_->is_connected();
      }
      if (!connected) {
        break;
      }
      std::this_thread::sleep_for(200ms);
    }
    BOOST_LOG(debug) << "Playnite IPC: client disconnected";
  }

  bool IpcClient::is_playnite_running() {
    try {
      auto v1 = platf::dxgi::find_process_ids_by_name(L"Playnite.DesktopApp.exe");
      if (!v1.empty()) {
        return true;
      }
      auto v2 = platf::dxgi::find_process_ids_by_name(L"Playnite.FullscreenApp.exe");
      return !v2.empty();
    } catch (...) {
      return false;
    }
  }

  bool IpcClient::send_json_line(const std::string &json) {
    std::scoped_lock lk(pipe_mutex_);
    if (!pipe_ || !pipe_->is_connected()) {
      BOOST_LOG(warning) << "Playnite IPC: send_json_line called but client not connected";
      return false;
    }
    std::string payload = json;
    payload.push_back('\n');
    auto bytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
    BOOST_LOG(debug) << "Playnite IPC: sending command (" << payload.size() << " bytes)";
    pipe_->send(bytes);
    return true;
  }

}  // namespace platf::playnite
