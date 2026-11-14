/**
 * @file src/platform/windows/playnite_ipc.h
 * @brief Playnite plugin IPC client using Windows named pipes with anonymous handshake.
 */
#pragma once

#include "src/logging.h"
#include "src/platform/windows/ipc/pipes.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>

namespace platf::playnite {

  /**
   * @brief IPC client that connects to the Playnite plugin's public pipe and receives messages.
   *
   * Client connects to the well-known public pipe exposed by the Playnite plugin. The plugin
   * hands out a per-session data pipe via the anonymous handshake, which we promote to an
   * asynchronous channel for JSON message exchange.
   */
  class IpcClient {
  public:
    // Optional override for control pipe name; empty selects the default well-known name.
    IpcClient();
    explicit IpcClient(const std::string &control_name);
    ~IpcClient();

    /**
     * @brief Start server thread if not already running.
     */
    void start();

    /**
     * @brief Stop server thread.
     */
    void stop();

    /**
     * @brief Set optional handler for raw plugin messages.
     */
    void set_message_handler(std::function<void(std::span<const uint8_t>)> handler) {
      handler_ = std::move(handler);
    }

    void set_connected_handler(std::function<void()> handler) {
      connected_handler_ = std::move(handler);
    }

    void set_disconnected_handler(std::function<void()> handler) {
      disconnected_handler_ = std::move(handler);
    }

    // Returns true if actively connected to the plugin
    bool is_active() const {
      return active_.load();
    }

    // Returns true if the server thread is running/listening (may not be connected yet)
    bool is_started() const {
      return running_.load();
    }

    // Send a JSON line (UTF-8 + trailing \n) to the plugin if connected
    bool send_json_line(const std::string &json);

  private:
    void run();
    void accumulate_and_dispatch_lines(std::span<const uint8_t> bytes);
    std::unique_ptr<platf::dxgi::INamedPipe> connect_to_plugin(unsigned long &error_out);
    void serve_connected_loop();
    // Sleep for the current backoff delay (with jitter), then grow it. Returns early on stop().
    void wait_before_retry();
    void sleep_while_running(std::chrono::milliseconds duration);
    // Log the first failure of a run, then at most one summary per interval.
    void note_connect_failure(const char *what, unsigned long error);
    void note_connect_success();
    // Check if any Playnite process is running (Desktop or Fullscreen)
    bool is_playnite_running();

    std::atomic<bool> running_ {false};
    std::thread worker_;
    std::unique_ptr<platf::dxgi::AsyncNamedPipe> pipe_;
    mutable std::mutex pipe_mutex_;
    std::function<void(std::span<const uint8_t>)> handler_;
    std::function<void()> connected_handler_;
    std::function<void()> disconnected_handler_;
    std::atomic<bool> broken_ {false};
    std::atomic<bool> active_ {false};
    std::string recv_buffer_;
    std::string control_name_;
    bool no_playnite_logged_ = false;  ///< Ensures we only log missing Playnite once until it appears.
    std::chrono::milliseconds retry_delay_ {0};  ///< Current reconnect backoff; 0 means the initial delay.
    bool failing_ = false;  ///< A connect failure was logged and no connect has succeeded since.
    unsigned failed_attempts_ = 0;  ///< Failed attempts since the last success.
    unsigned failures_since_summary_ = 0;
    std::chrono::steady_clock::time_point last_failure_summary_ {};
  };
}  // namespace platf::playnite
