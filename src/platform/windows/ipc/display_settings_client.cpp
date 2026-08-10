/**
 * @file src/platform/windows/ipc/display_settings_client.cpp
 */
#ifdef _WIN32

  // standard
  #include <algorithm>
  #include <array>
  #include <atomic>
  #include <chrono>
  #include <cstdint>
  #include <deque>
  #include <functional>
  #include <limits>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <span>
  #include <string>
  #include <utility>
  #include <vector>

  #include <nlohmann/json.hpp>

  // local
  #include "display_settings_client.h"
  #include "src/globals.h"
  #include "src/logging.h"
  #include "src/platform/windows/display_helper_v2/timing.h"
  #include "src/platform/windows/ipc/pipes.h"

namespace platf::display_helper_client {

  namespace {
    constexpr int kConnectTimeoutMs = 2000;
    constexpr int kSendTimeoutMs = 5000;
    constexpr int kShutdownIpcTimeoutMs = 500;
    constexpr int kV2ApplyResultTimeoutMs = static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        display_helper::v2::timing::kApplyStartupBudget).count());
    constexpr int kLegacyApplyResultTimeoutMs = 5000;
    // A refresh-only mode set on a virtual display serializes against the OS display
    // stack. While an alt-tab is already changing modes, the helper has been measured
    // at ~6s for a single apply, so a 5s budget declared a healthy helper dead and
    // retired a connection it was about to answer.
    constexpr int kRefreshRateResultTimeoutMs = 12000;
    // Best-effort adaptive refresh has its own retry, so it must never pay the full
    // connect budget (anonymous transport, then named fallback) when the helper is
    // down: that stalls the poll loop for seconds per attempt.
    constexpr int kRefreshRateConnectTimeoutMs = 600;
    // Per-stage cap for SNAPSHOT_CURRENT. A bounded snapshot shares the caller's
    // stream-start budget, so without its own cap a slow snapshot can consume the
    // whole budget and leave nothing for APPLY.
    constexpr int kSnapshotResultTimeoutMs = 3000;
    // Retiring a stale connection is a lock acquisition, not helper work, so it
    // gets its own short budget: the deadlines that lead here are already spent.
    constexpr int kStaleConnectionRetireTimeoutMs = 250;

    bool shutdown_requested() {
      if (!mail::man) {
        return false;
      }
      try {
        auto shutdown_event = mail::man->event<bool>(mail::shutdown);
        return shutdown_event && shutdown_event->peek();
      } catch (...) {
        return false;
      }
    }

    int effective_connect_timeout() {
      return shutdown_requested() ? kShutdownIpcTimeoutMs : kConnectTimeoutMs;
    }

    int effective_send_timeout() {
      return shutdown_requested() ? kShutdownIpcTimeoutMs : kSendTimeoutMs;
    }

  }  // namespace

  /**
   * @brief IPC message types used by the display settings helper protocol.
   */
  enum class MsgType : uint8_t {
    Apply = 1,  ///< Apply display settings from JSON payload.
    Revert = 2,  ///< Revert display settings to the previous state.
    Reset = 3,  ///< Reset helper persistence/state (if supported).
    ExportGolden = 4,  ///< Export current OS settings as golden snapshot
    LogLevel = 5,  ///< Update helper log level (payload: [u8 min_log_level]); v2 engine only.
    ApplyResult = 6,  ///< Helper acknowledgement for APPLY (payload: [u8 success][optional message...]).
    Disarm = 7,  ///< Cancel any pending restore/watchdog actions on the helper.
    SnapshotCurrent = 8,  ///< Save current session snapshot (rotate current->previous) without applying config.
    VerificationResult = 9,  ///< Helper acknowledgement for verification completion (payload: [u8 success]); v2 engine only.
    RefreshRate = 10,  ///< Change only one display's refresh rate.
    RefreshRateResult = 11,  ///< Helper acknowledgement for RefreshRate (payload: [u8 success]).
    SnapshotResult = 12,  ///< Helper acknowledgement for SnapshotCurrent (payload: [u8 success]).
    Ping = 0xFE,  ///< Health check message; expects a response.
    Stop = 0xFF  ///< Request helper process to terminate gracefully.
  };

  namespace {
    enum class ApplyResponseProtocol {
      Unknown,
      Legacy,
      V2,
    };

    struct ApplyResult {
      bool success = false;
      ApplyResponseProtocol protocol = ApplyResponseProtocol::Unknown;
    };

    using PipePtr = std::shared_ptr<platf::dxgi::INamedPipe>;

    // The transport, its detected capability, and its buffered replies are a
    // single ownership unit. A retired connection may still have a reader
    // finishing a short receive slice, but it can never clear or reclassify
    // the reply state of the connection that replaced it.
    struct ConnectionSession {
      ConnectionSession(PipePtr pipe_in, std::uint64_t generation_in)
        : pipe(std::move(pipe_in)), generation(generation_in) {}

      PipePtr pipe;
      const std::uint64_t generation;
      std::atomic<ApplyResponseProtocol> protocol {ApplyResponseProtocol::Unknown};
      // An untagged response on an unknown or legacy helper cannot safely
      // cross a superseding command. The control path retires this session
      // before it sends the successor, then discovers v1/v2 on the fresh
      // connection.
      std::atomic<bool> untagged_response_pending {false};
      std::timed_mutex response_mutex;
      std::mutex response_inbox_mutex;
      std::deque<std::vector<uint8_t>> response_inbox;
    };

    using SessionPtr = std::shared_ptr<ConnectionSession>;

    constexpr std::size_t kMaxBufferedResponses = 32;
    constexpr std::size_t kMaxIssuedApplyRequestIds = 64;

    std::mutex &issued_apply_request_ids_mutex() {
      static std::mutex m;
      return m;
    }

    std::deque<std::uint64_t> &issued_apply_request_ids() {
      static std::deque<std::uint64_t> ids;
      return ids;
    }

    void remember_issued_apply_request_id(std::uint64_t request_id) {
      std::lock_guard<std::mutex> lock(issued_apply_request_ids_mutex());
      auto &ids = issued_apply_request_ids();
      if (ids.size() == kMaxIssuedApplyRequestIds) {
        ids.pop_front();
      }
      ids.push_back(request_id);
    }

    bool was_issued_apply_request_id(std::uint64_t request_id) {
      std::lock_guard<std::mutex> lock(issued_apply_request_ids_mutex());
      const auto &ids = issued_apply_request_ids();
      return std::find(ids.begin(), ids.end(), request_id) != ids.end();
    }

    bool is_bufferable_response(uint8_t type) {
      return type == static_cast<uint8_t>(MsgType::ApplyResult) ||
             type == static_cast<uint8_t>(MsgType::VerificationResult) ||
             type == static_cast<uint8_t>(MsgType::RefreshRateResult) ||
             type == static_cast<uint8_t>(MsgType::SnapshotResult);
    }

    void buffer_response(const SessionPtr &session, std::span<const uint8_t> bytes) {
      if (!session || bytes.empty() || !is_bufferable_response(bytes.front())) {
        return;
      }
      std::lock_guard<std::mutex> lock(session->response_inbox_mutex);
      auto &inbox = session->response_inbox;
      if (inbox.size() == kMaxBufferedResponses) {
        inbox.pop_front();
      }
      inbox.emplace_back(bytes.begin(), bytes.end());
    }

    ApplyResponseProtocol current_apply_response_protocol(const SessionPtr &session) {
      return session ? session->protocol.load(std::memory_order_acquire) : ApplyResponseProtocol::Unknown;
    }

    void remember_apply_response_protocol(const SessionPtr &session, ApplyResponseProtocol protocol) {
      if (!session || protocol == ApplyResponseProtocol::Unknown) {
        return;
      }
      auto expected = ApplyResponseProtocol::Unknown;
      (void) session->protocol.compare_exchange_strong(
        expected,
        protocol,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
    }

    std::optional<std::vector<uint8_t>> take_buffered_response(
      const SessionPtr &session,
      MsgType type,
      const std::function<bool(std::span<const uint8_t>)> &matches) {
      if (!session) {
        return std::nullopt;
      }
      std::lock_guard<std::mutex> lock(session->response_inbox_mutex);
      auto &inbox = session->response_inbox;
      const auto it = std::find_if(inbox.begin(), inbox.end(), [&](const std::vector<uint8_t> &response) {
        return !response.empty() &&
               response.front() == static_cast<uint8_t>(type) &&
               matches(response);
      });
      if (it == inbox.end()) {
        return std::nullopt;
      }
      auto bytes = std::move(*it);
      inbox.erase(it);
      return bytes;
    }

    std::optional<std::uint64_t> response_request_id(std::span<const uint8_t> bytes) {
      if (bytes.size() < 10) {
        return std::nullopt;
      }
      std::uint64_t request_id = 0;
      for (unsigned int shift = 0; shift < 64; shift += 8) {
        request_id |= static_cast<std::uint64_t>(bytes[2 + (shift / 8)]) << shift;
      }
      return request_id;
    }

    std::optional<ApplyResult> decode_apply_response(
      std::span<const uint8_t> bytes,
      std::uint64_t expected_request_id,
      ApplyResponseProtocol protocol) {
      if (bytes.size() < 2) {
        return std::nullopt;
      }
      if (protocol == ApplyResponseProtocol::Legacy) {
        return ApplyResult {.success = bytes[1] != 0, .protocol = ApplyResponseProtocol::Legacy};
      }
      const auto request_id = response_request_id(bytes);
      if (protocol == ApplyResponseProtocol::V2) {
        if (!request_id || *request_id != expected_request_id) {
          return std::nullopt;
        }
        return ApplyResult {.success = bytes[1] != 0, .protocol = ApplyResponseProtocol::V2};
      }

      if (!request_id) {
        return ApplyResult {.success = bytes[1] != 0, .protocol = ApplyResponseProtocol::Legacy};
      }
      if (*request_id == expected_request_id) {
        return ApplyResult {.success = bytes[1] != 0, .protocol = ApplyResponseProtocol::V2};
      }
      // A tagged response for an earlier request proves that this is v2
      // traffic, even when that older request was cancelled. Do not mistake a
      // stale failed v2 reply for a legacy failure just because its token is
      // followed by an error string.
      if (was_issued_apply_request_id(*request_id)) {
        return std::nullopt;
      }
      // A legacy failure may carry a long human-readable error string, which
      // has no token but can be at least eight bytes long. There is no earlier
      // v2 request on an unknown transport, so treat that first failure as the
      // legacy reply rather than waiting for a token that will never arrive.
      if (bytes[1] == 0) {
        return ApplyResult {.success = false, .protocol = ApplyResponseProtocol::Legacy};
      }
      return std::nullopt;
    }

    bool matches_verification_response(std::span<const uint8_t> bytes, std::uint64_t expected_request_id) {
      const auto request_id = response_request_id(bytes);
      return request_id && *request_id == expected_request_id;
    }

    std::optional<ApplyResult> wait_for_apply_result_locked(
      const SessionPtr &session,
      std::uint64_t expected_request_id,
      ApplyResponseProtocol protocol,
      int timeout_ms,
      const std::function<bool()> &cancellation_predicate
    ) {
      using namespace std::chrono;

      if (!session || !session->pipe) {
        return std::nullopt;
      }

      const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
      std::array<uint8_t, 2048> buffer {};

      while (steady_clock::now() < deadline) {
        if (cancellation_predicate && cancellation_predicate()) {
          BOOST_LOG(debug) << "Display helper IPC: APPLY result wait cancelled because the connection was retired";
          return std::nullopt;
        }

        if (auto buffered = take_buffered_response(
              session,
              MsgType::ApplyResult,
              [expected_request_id, protocol](std::span<const uint8_t> bytes) {
                return decode_apply_response(bytes, expected_request_id, protocol).has_value();
              })) {
          const auto result = decode_apply_response(
            std::span<const uint8_t>(buffered->data(), buffered->size()),
            expected_request_id,
            protocol);
          if (!result) {
            continue;
          }
          remember_apply_response_protocol(session, result->protocol);
          session->untagged_response_pending.store(false, std::memory_order_release);
          const std::size_t message_offset = result->protocol == ApplyResponseProtocol::V2 ? 10 : 2;
          if (!result->success && buffered->size() > message_offset) {
            std::string helper_msg(
              reinterpret_cast<const char *>(buffered->data() + message_offset),
              reinterpret_cast<const char *>(buffered->data() + buffered->size()));
            BOOST_LOG(error) << "Display helper reported APPLY failure: " << helper_msg;
          }
          return result;
        }

        const auto now = steady_clock::now();
        auto remaining = duration_cast<milliseconds>(deadline - now);
        if (remaining.count() < 0) {
          remaining = milliseconds(0);
        }
        const int timeout_ms = static_cast<int>(std::clamp<long long>(remaining.count(), 1LL, 100LL));
        size_t bytes_read = 0;
        auto result = session->pipe->receive(buffer, bytes_read, timeout_ms);

        if (result == platf::dxgi::PipeResult::Timeout) {
          continue;
        }
        if (cancellation_predicate && cancellation_predicate()) {
          return std::nullopt;
        }
        if (result != platf::dxgi::PipeResult::Success) {
          BOOST_LOG(error) << "Display helper IPC: failed waiting for APPLY result (pipe error)";
          return std::nullopt;
        }
        if (bytes_read == 0) {
          BOOST_LOG(error) << "Display helper IPC: connection closed while waiting for APPLY result";
          return std::nullopt;
        }

        const uint8_t msg_type = buffer[0];
        if (msg_type == static_cast<uint8_t>(MsgType::ApplyResult)) {
          const std::span<const uint8_t> frame(buffer.data(), bytes_read);
          const auto decoded = decode_apply_response(frame, expected_request_id, protocol);
          if (!decoded) {
            const auto response_id = response_request_id(frame);
            BOOST_LOG(debug) << "Display helper IPC: preserving APPLY result for request="
                             << (response_id ? std::to_string(*response_id) : std::string {"legacy"});
            buffer_response(session, frame);
            continue;
          }
          remember_apply_response_protocol(session, decoded->protocol);
          session->untagged_response_pending.store(false, std::memory_order_release);
          const std::size_t message_offset = decoded->protocol == ApplyResponseProtocol::V2 ? 10 : 2;
          if (!decoded->success && bytes_read > message_offset) {
            std::string helper_msg(
              reinterpret_cast<const char *>(buffer.data() + message_offset),
              reinterpret_cast<const char *>(buffer.data() + bytes_read));
            BOOST_LOG(error) << "Display helper reported APPLY failure: " << helper_msg;
          }
          return decoded;
        }

        if (msg_type == static_cast<uint8_t>(MsgType::Ping)) {
          continue;
        }
        if (is_bufferable_response(msg_type)) {
          buffer_response(session, std::span<const uint8_t>(buffer.data(), bytes_read));
          continue;
        }

        BOOST_LOG(debug) << "Display helper IPC: ignoring unexpected message type=" << static_cast<int>(msg_type)
                         << " while awaiting APPLY result";
      }

      BOOST_LOG(error) << "Display helper IPC: timed out waiting for APPLY result acknowledgement";
      return std::nullopt;
    }

    std::optional<bool> wait_for_verification_result_locked(
      const SessionPtr &session,
      int timeout_ms,
      std::uint64_t expected_request_id,
      const std::function<bool()> &cancellation_predicate
    ) {
      using namespace std::chrono;

      if (timeout_ms <= 0) {
        return std::nullopt;
      }
      if (!session || !session->pipe) {
        return std::nullopt;
      }

      const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
      std::array<uint8_t, 2048> buffer {};

      while (steady_clock::now() < deadline) {
        if (cancellation_predicate && cancellation_predicate()) {
          BOOST_LOG(debug) << "Display helper IPC: verification wait cancelled by a newer APPLY";
          return std::nullopt;
        }

        if (auto buffered = take_buffered_response(
              session,
              MsgType::VerificationResult,
              [expected_request_id](std::span<const uint8_t> bytes) {
                return matches_verification_response(bytes, expected_request_id);
              })) {
          if (cancellation_predicate && cancellation_predicate()) {
            return std::nullopt;
          }
          return (*buffered)[1] != 0;
        }

        const auto now = steady_clock::now();
        auto remaining = duration_cast<milliseconds>(deadline - now);
        if (remaining.count() < 0) {
          remaining = milliseconds(0);
        }
        // Keep the receive bounded so an older stream-start gate can notice a
        // superseding APPLY and release the single response reader promptly.
        const int wait_ms = static_cast<int>(std::clamp<long long>(remaining.count(), 1LL, 100LL));
        size_t bytes_read = 0;
        auto result = session->pipe->receive(buffer, bytes_read, wait_ms);

        if (result == platf::dxgi::PipeResult::Timeout) {
          continue;
        }
        if (cancellation_predicate && cancellation_predicate()) {
          return std::nullopt;
        }
        if (result != platf::dxgi::PipeResult::Success) {
          BOOST_LOG(error) << "Display helper IPC: failed waiting for verification result (pipe error)";
          return std::nullopt;
        }
        if (bytes_read == 0) {
          BOOST_LOG(error) << "Display helper IPC: connection closed while waiting for verification result";
          return std::nullopt;
        }

        const uint8_t msg_type = buffer[0];
        if (msg_type == static_cast<uint8_t>(MsgType::VerificationResult)) {
          const std::span<const uint8_t> frame(buffer.data(), bytes_read);
          if (bytes_read < 10) {
            BOOST_LOG(warning) << "Display helper IPC: ignoring uncorrelated verification result from an older helper protocol.";
            continue;
          }
          if (!matches_verification_response(frame, expected_request_id)) {
            const auto response_id = response_request_id(frame);
            BOOST_LOG(debug) << "Display helper IPC: preserving verification result for request="
                             << (response_id ? std::to_string(*response_id) : std::string {"unknown"});
            buffer_response(session, frame);
            continue;
          }
          if (cancellation_predicate && cancellation_predicate()) {
            return std::nullopt;
          }
          return buffer[1] != 0;
        }

        if (msg_type == static_cast<uint8_t>(MsgType::Ping)) {
          continue;
        }
        if (is_bufferable_response(msg_type)) {
          buffer_response(session, std::span<const uint8_t>(buffer.data(), bytes_read));
          continue;
        }

        BOOST_LOG(debug) << "Display helper IPC: ignoring unexpected message type=" << static_cast<int>(msg_type)
                         << " while awaiting verification result";
      }

      BOOST_LOG(error) << "Display helper IPC: timed out waiting for verification result acknowledgement";
      return std::nullopt;
    }

    std::optional<bool> wait_for_refresh_rate_result_locked(
      const SessionPtr &session,
      std::optional<std::uint64_t> expected_request_id,
      const std::function<bool()> &cancellation_predicate
    ) {
      using namespace std::chrono;

      if (!session || !session->pipe) {
        return std::nullopt;
      }
      const auto deadline = steady_clock::now() + milliseconds(kRefreshRateResultTimeoutMs);
      std::array<uint8_t, 2048> buffer {};
      while (steady_clock::now() < deadline) {
        if (cancellation_predicate && cancellation_predicate()) {
          return std::nullopt;
        }
        if (auto buffered = take_buffered_response(
              session,
              MsgType::RefreshRateResult,
              [expected_request_id](std::span<const uint8_t> bytes) {
                if (bytes.size() < 2) {
                  return false;
                }
                const auto response_id = response_request_id(bytes);
                return expected_request_id ? response_id && *response_id == *expected_request_id : !response_id;
              })) {
          if (cancellation_predicate && cancellation_predicate()) {
            return std::nullopt;
          }
          return (*buffered)[1] != 0;
        }
        const auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now());
        size_t bytes_read = 0;
        const auto result = session->pipe->receive(
          buffer,
          bytes_read,
          static_cast<int>(std::clamp<long long>(remaining.count(), 1LL, 100LL))
        );
        if (result == platf::dxgi::PipeResult::Timeout) {
          continue;
        }
        if (cancellation_predicate && cancellation_predicate()) {
          return std::nullopt;
        }
        if (result != platf::dxgi::PipeResult::Success || bytes_read == 0) {
          return std::nullopt;
        }

        const auto msg_type = buffer[0];
        if (msg_type == static_cast<uint8_t>(MsgType::RefreshRateResult)) {
          const std::span<const uint8_t> frame(buffer.data(), bytes_read);
          const auto response_id = response_request_id(frame);
          const bool matches = bytes_read >= 2 &&
                               (expected_request_id ? response_id && *response_id == *expected_request_id : !response_id);
          if (matches) {
            return buffer[1] != 0;
          }
          if (response_id) {
            buffer_response(session, frame);
          } else {
            BOOST_LOG(debug) << "Display helper IPC: ignoring uncorrelated legacy refresh-rate result.";
          }
          continue;
        }
        if (msg_type == static_cast<uint8_t>(MsgType::Ping)) {
          continue;
        }
        if (is_bufferable_response(msg_type)) {
          buffer_response(session, std::span<const uint8_t>(buffer.data(), bytes_read));
          continue;
        }
      }
      return std::nullopt;
    }

    std::optional<bool> wait_for_snapshot_result_locked(
      const SessionPtr &session,
      int timeout_ms,
      std::uint64_t expected_request_id,
      const std::function<bool()> &cancellation_predicate
    ) {
      using namespace std::chrono;

      if (timeout_ms <= 0) {
        return std::nullopt;
      }
      if (!session || !session->pipe || expected_request_id == 0) {
        return std::nullopt;
      }

      const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
      std::array<uint8_t, 2048> buffer {};
      while (steady_clock::now() < deadline) {
        if (cancellation_predicate && cancellation_predicate()) {
          return std::nullopt;
        }
        if (auto buffered = take_buffered_response(
              session,
              MsgType::SnapshotResult,
              [expected_request_id](std::span<const uint8_t> bytes) {
                const auto response_id = response_request_id(bytes);
                return bytes.size() >= 2 && response_id && *response_id == expected_request_id;
              })) {
          if (cancellation_predicate && cancellation_predicate()) {
            return std::nullopt;
          }
          return (*buffered)[1] != 0;
        }
        const auto now = steady_clock::now();
        auto remaining = duration_cast<milliseconds>(deadline - now);
        if (remaining.count() < 0) {
          remaining = milliseconds(0);
        }
        size_t bytes_read = 0;
        const auto result = session->pipe->receive(
          buffer,
          bytes_read,
          static_cast<int>(std::clamp<long long>(remaining.count(), 1LL, 100LL))
        );
        if (result == platf::dxgi::PipeResult::Timeout) {
          continue;
        }
        if (cancellation_predicate && cancellation_predicate()) {
          return std::nullopt;
        }
        if (result != platf::dxgi::PipeResult::Success || bytes_read == 0) {
          BOOST_LOG(error) << "Display helper IPC: failed waiting for SNAPSHOT_CURRENT result";
          return std::nullopt;
        }

        const auto msg_type = buffer[0];
        if (msg_type == static_cast<uint8_t>(MsgType::SnapshotResult)) {
          const std::span<const uint8_t> frame(buffer.data(), bytes_read);
          const auto response_id = response_request_id(frame);
          if (bytes_read >= 2 && response_id && *response_id == expected_request_id) {
            return buffer[1] != 0;
          }
          if (response_id) {
            buffer_response(session, frame);
          } else {
            BOOST_LOG(debug) << "Display helper IPC: ignoring uncorrelated legacy SNAPSHOT_CURRENT result.";
          }
          continue;
        }
        if (msg_type == static_cast<uint8_t>(MsgType::Ping)) {
          continue;
        }
        if (is_bufferable_response(msg_type)) {
          buffer_response(session, std::span<const uint8_t>(buffer.data(), bytes_read));
          continue;
        }

        BOOST_LOG(debug) << "Display helper IPC: ignoring unexpected message type=" << static_cast<int>(msg_type)
                         << " while awaiting SNAPSHOT_CURRENT result";
      }

      BOOST_LOG(error) << "Display helper IPC: timed out waiting for SNAPSHOT_CURRENT result";
      return std::nullopt;
    }

    void append_u32_le(std::vector<uint8_t> &payload, std::uint32_t value) {
      payload.push_back(static_cast<uint8_t>(value & 0xffu));
      payload.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
      payload.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
      payload.push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
    }

    void append_u64_le(std::vector<uint8_t> &payload, std::uint64_t value) {
      for (unsigned int shift = 0; shift < 64; shift += 8) {
        payload.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
      }
    }

    constexpr std::uint32_t kV2RefreshRateMagic = 0x32524653u;  // "SFR2" in little-endian bytes.
  }  // namespace

  static bool send_message(
    platf::dxgi::INamedPipe &pipe,
    MsgType type,
    const std::vector<uint8_t> &payload,
    std::optional<int> send_timeout_override_ms = std::nullopt
  ) {
    const bool is_ping = (type == MsgType::Ping || type == MsgType::LogLevel);
    if (!is_ping) {
      BOOST_LOG(info) << "Display helper IPC: sending frame type=" << static_cast<int>(type)
                      << ", payload_len=" << payload.size();
    }
    std::vector<uint8_t> out;
    out.reserve(1 + payload.size());
    out.push_back(static_cast<uint8_t>(type));
    out.insert(out.end(), payload.begin(), payload.end());
    const int timeout_ms = send_timeout_override_ms.value_or(effective_send_timeout());
    const bool ok = pipe.send(out, timeout_ms);
    if (!is_ping) {
      BOOST_LOG(info) << "Display helper IPC: send result=" << (ok ? "true" : "false");
    }
    return ok;
  }

  // Persistent connection across a stream session. Helper stays alive until
  // successful revert; we reuse the data pipe for APPLY/REVERT. The session
  // owns all response-routing state so an old reader cannot affect a newer
  // connection during reconnect/retirement.
  static SessionPtr &session_singleton() {
    static SessionPtr session;
    return session;
  }

  // The active session's unique generation is part of every v2 APPLY ticket.
  // Zero denotes no active session. A separate allocator prevents ABA when a
  // transport is retired and recreated rapidly.
  static std::atomic<std::uint64_t> &connection_generation() {
    static std::atomic<std::uint64_t> generation {0};
    return generation;
  }

  static std::atomic<std::uint64_t> &next_connection_generation() {
    static std::atomic<std::uint64_t> generation {1};
    return generation;
  }

  static std::uint64_t allocate_connection_generation() {
    return next_connection_generation().fetch_add(1, std::memory_order_acq_rel);
  }

  static bool session_is_current(const SessionPtr &session) {
    return session &&
           connection_generation().load(std::memory_order_acquire) == session->generation;
  }

  static SessionPtr retire_session_locked() {
    auto retired = std::move(session_singleton());
    if (retired) {
      connection_generation().store(0, std::memory_order_release);
    }
    return retired;
  }

  // Called under write_mutex then connection_mutex. An unknown or legacy
  // helper can have exactly one untagged reply in flight. A superseding
  // control operation must use a new pipe; otherwise that reply can either be
  // mistaken for the successor's legacy acknowledgement or keep its response
  // lane blocked until the old operation times out.
  static SessionPtr retire_untagged_response_session_if_needed_locked() {
    const auto &session = session_singleton();
    if (!session ||
        session->protocol.load(std::memory_order_acquire) == ApplyResponseProtocol::V2 ||
        !session->untagged_response_pending.exchange(false, std::memory_order_acq_rel)) {
      return {};
    }
    BOOST_LOG(debug) << "Display helper IPC: retiring connection before superseding an untagged response.";
    return retire_session_locked();
  }

  static std::optional<int> &last_log_level_sent() {
    static std::optional<int> level;
    return level;
  }

  static std::mutex &log_level_mutex() {
    static std::mutex m;
    return m;
  }

  static void reset_log_level_cache() {
    std::lock_guard<std::mutex> lock(log_level_mutex());
    last_log_level_sent().reset();
  }

  static std::atomic<std::uint64_t> &next_apply_request_id() {
    static std::atomic<std::uint64_t> id {1};
    return id;
  }

  static std::atomic<std::uint64_t> &next_auxiliary_request_id() {
    static std::atomic<std::uint64_t> id {1};
    return id;
  }

  // Incrementing this token releases an older APPLY waiter before it can
  // consume a reply for the command that superseded or disarmed it.
  static std::atomic<std::uint64_t> &apply_wait_generation() {
    static std::atomic<std::uint64_t> generation {0};
    return generation;
  }

  // Connection ownership, byte-pipe writes, and response reads have different
  // lifetimes. In particular, an APPLY can wait up to the staged operation
  // envelope while heartbeat/control writes retain a short write lease.
  static std::timed_mutex &connection_mutex() {
    static std::timed_mutex m;
    return m;
  }

  static std::timed_mutex &write_mutex() {
    static std::timed_mutex m;
    return m;
  }

  static int remaining_timeout_ms(const std::chrono::steady_clock::time_point &deadline);

  // A cancellation/control generation is ordered with the short command
  // write lease. If the active helper has not yet identified its protocol,
  // retire it in the same critical section so an untagged legacy reply cannot
  // cross this control boundary.
  static std::optional<std::uint64_t> cancel_or_begin_apply_wait_cancellable(const std::function<bool()> &cancelled) {
    SessionPtr retired;
    std::uint64_t generation = 0;
    {
      std::unique_lock<std::timed_mutex> write_lock(write_mutex(), std::defer_lock);
      while (!write_lock.try_lock_for(std::chrono::milliseconds(100))) {
        if (cancelled && cancelled()) {
          return std::nullopt;
        }
      }
      if (cancelled && cancelled()) {
        return std::nullopt;
      }
      std::unique_lock<std::timed_mutex> connection_lock(connection_mutex(), std::defer_lock);
      while (!connection_lock.try_lock_for(std::chrono::milliseconds(100))) {
        if (cancelled && cancelled()) {
          return std::nullopt;
        }
      }
      if (cancelled && cancelled()) {
        return std::nullopt;
      }
      retired = retire_untagged_response_session_if_needed_locked();
      generation = apply_wait_generation().fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    if (retired) {
      reset_log_level_cache();
    }
    return generation;
  }

  static std::uint64_t cancel_or_begin_apply_wait() {
    // The non-cancellable command paths preserve their existing blocking
    // serialization semantics. The helper always returns a generation when
    // no predicate is supplied.
    return *cancel_or_begin_apply_wait_cancellable({});
  }

  // Ensure connected while holding connection_mutex(). Returns true on success.
  static bool ensure_connected_locked(
    std::optional<int> connect_timeout_override_ms = std::nullopt,
    const std::function<bool()> &cancelled = {},
    const std::chrono::steady_clock::time_point operation_deadline =
      std::chrono::steady_clock::time_point::max()) {
    if (shutdown_requested() || (cancelled && cancelled())) {
      return false;
    }
    auto &session = session_singleton();
    if (session && session->pipe && session->pipe->is_connected()) {
      return true;
    }
    if (session) {
      (void) retire_session_locked();
      reset_log_level_cache();
    }

    BOOST_LOG(debug) << "Display helper IPC: connecting to server pipe 'sunshine_display_helper'";
    const int connect_timeout_ms = connect_timeout_override_ms.value_or(effective_connect_timeout());
    const auto connect_deadline = std::min(
      std::chrono::steady_clock::now() + std::chrono::milliseconds(connect_timeout_ms),
      operation_deadline
    );
    auto remaining_ms = [&]() -> int {
      return remaining_timeout_ms(connect_deadline);
    };

    auto create_session = [&](auto creator) -> bool {
      if ((cancelled && cancelled()) || remaining_ms() <= 0) {
        return false;
      }
      auto pipe = std::make_shared<platf::dxgi::SelfHealingPipe>(std::move(creator));
      if (!pipe) {
        return false;
      }
      pipe->wait_for_client_connection(remaining_ms());
      if ((cancelled && cancelled()) ||
          remaining_ms() <= 0 ||
          !pipe->is_connected()) {
        return false;
      }
      const auto generation = allocate_connection_generation();
      session = std::make_shared<ConnectionSession>(std::move(pipe), generation);
      connection_generation().store(generation, std::memory_order_release);
      return true;
    };

    // Create fresh pipe - try anonymous first, then named fallback. The
    // normal connection path is allowed to negotiate the anonymous transport;
    // fast paths below intentionally never call this function.
    if (remaining_ms() > 0 && create_session([]() -> std::unique_ptr<platf::dxgi::INamedPipe> {
          platf::dxgi::FramedPipeFactory ff(std::make_unique<platf::dxgi::AnonymousPipeFactory>());
          return ff.create_client("sunshine_display_helper");
        })) {
      return true;
    }
    if (remaining_ms() > 0) {
      BOOST_LOG(debug) << "Display helper IPC: anonymous connect failed; trying named fallback";
      if (create_session([]() -> std::unique_ptr<platf::dxgi::INamedPipe> {
            platf::dxgi::FramedPipeFactory ff(std::make_unique<platf::dxgi::NamedPipeFactory>());
            return ff.create_client("sunshine_display_helper");
          })) {
        return true;
      }
    }
    BOOST_LOG(warning) << "Display helper IPC: connection failed";
    return false;
  }

  static SessionPtr connected_session(
    std::optional<int> connect_timeout_override_ms = std::nullopt,
    const std::function<bool()> &cancelled = {}) {
    std::unique_lock<std::timed_mutex> lock(connection_mutex(), std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(100))) {
      if (cancelled && cancelled()) {
        return {};
      }
    }
    if (cancelled && cancelled()) {
      return {};
    }
    if (!ensure_connected_locked(connect_timeout_override_ms, cancelled)) {
      return {};
    }
    return session_singleton();
  }

  static SessionPtr connected_session_within(
    const std::chrono::steady_clock::time_point &deadline,
    const int connect_timeout_cap_ms,
    const std::function<bool()> &cancelled = {}
  ) {
    std::unique_lock<std::timed_mutex> lock(connection_mutex(), std::defer_lock);
    while (true) {
      if (cancelled && cancelled()) {
        return {};
      }
      const int remaining = remaining_timeout_ms(deadline);
      if (remaining <= 0) {
        return {};
      }
      if (lock.try_lock_for(std::chrono::milliseconds(std::min(remaining, 100)))) {
        break;
      }
    }
    if (cancelled && cancelled()) {
      return {};
    }
    const int remaining = remaining_timeout_ms(deadline);
    if (remaining <= 0 ||
        !ensure_connected_locked(
          std::min(connect_timeout_cap_ms, remaining),
          cancelled,
          deadline)) {
      return {};
    }
    return session_singleton();
  }

  static SessionPtr current_session() {
    std::lock_guard<std::timed_mutex> lock(connection_mutex());
    return session_singleton();
  }

  // Fast recovery probes are deliberately cache-only. Anonymous-pipe creation
  // can take seconds for handshake/fallback, so it cannot participate in a
  // 100-150ms disarm/ping budget.
  static SessionPtr cached_connected_session_within(const std::chrono::steady_clock::time_point &deadline) {
    const int lock_timeout_ms = remaining_timeout_ms(deadline);
    if (lock_timeout_ms <= 0) {
      return {};
    }
    std::unique_lock<std::timed_mutex> lock(connection_mutex(), std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(lock_timeout_ms))) {
      return {};
    }
    const auto session = session_singleton();
    if (!session || !session->pipe || !session->pipe->is_connected()) {
      return {};
    }
    return session;
  }

  static int remaining_timeout_ms(const std::chrono::steady_clock::time_point &deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    return static_cast<int>(std::clamp<long long>(remaining, 0LL, static_cast<long long>(std::numeric_limits<int>::max())));
  }

  static bool lock_response_reader_until(
    const SessionPtr &session,
    std::unique_lock<std::timed_mutex> &lock,
    const std::chrono::steady_clock::time_point &deadline,
    const std::function<bool()> &cancelled = {}) {
    if (!session) {
      return false;
    }
    lock = std::unique_lock<std::timed_mutex>(session->response_mutex, std::defer_lock);
    while (true) {
      if ((cancelled && cancelled()) || !session_is_current(session)) {
        return false;
      }
      const int remaining = remaining_timeout_ms(deadline);
      if (remaining <= 0) {
        return false;
      }
      if (lock.try_lock_for(std::chrono::milliseconds(std::min(remaining, 100)))) {
        if ((cancelled && cancelled()) || !session_is_current(session)) {
          lock.unlock();
          return false;
        }
        return true;
      }
    }
  }

  static std::optional<std::uint64_t> cancel_or_begin_apply_wait_within(
    const std::chrono::steady_clock::time_point &deadline,
    const std::function<bool()> &cancelled = {}) {
    std::unique_lock<std::timed_mutex> write_lock(write_mutex(), std::defer_lock);
    while (true) {
      if (cancelled && cancelled()) {
        return std::nullopt;
      }
      const int remaining = remaining_timeout_ms(deadline);
      if (remaining <= 0) {
        return std::nullopt;
      }
      if (write_lock.try_lock_for(std::chrono::milliseconds(std::min(remaining, 100)))) {
        break;
      }
    }

    SessionPtr retired;
    // session_singleton() is owned by connection_mutex(). Acquire the normal
    // write -> connection order even when no retirement is needed; otherwise
    // a fast DISARM can race reconnect/reset while inspecting the shared_ptr.
    std::unique_lock<std::timed_mutex> connection_lock(connection_mutex(), std::defer_lock);
    while (true) {
      if (cancelled && cancelled()) {
        return std::nullopt;
      }
      const int remaining = remaining_timeout_ms(deadline);
      if (remaining <= 0) {
        return std::nullopt;
      }
      if (connection_lock.try_lock_for(std::chrono::milliseconds(std::min(remaining, 100)))) {
        break;
      }
    }
    if ((cancelled && cancelled()) || remaining_timeout_ms(deadline) <= 0) {
      return std::nullopt;
    }
    retired = retire_untagged_response_session_if_needed_locked();
    const auto generation = apply_wait_generation().fetch_add(1, std::memory_order_acq_rel) + 1;
    if (retired) {
      reset_log_level_cache();
    }
    return generation;
  }

  static bool send_serialized(
    const SessionPtr &session,
    MsgType type,
    const std::vector<uint8_t> &payload,
    std::optional<int> send_timeout_override_ms = std::nullopt,
    std::optional<std::uint64_t> expected_apply_generation = std::nullopt,
    std::uint64_t *connection_generation_out = nullptr,
    bool begins_untagged_response = false,
    const std::function<bool()> &cancelled = {}
  ) {
    if (!session || !session->pipe) {
      return false;
    }
    std::unique_lock<std::timed_mutex> write_lock(write_mutex(), std::defer_lock);
    while (!write_lock.try_lock_for(std::chrono::milliseconds(100))) {
      if (cancelled && cancelled()) {
        return false;
      }
    }
    if (cancelled && cancelled()) {
      return false;
    }
    // Serialize command ownership with connection retirement. Hold the
    // connection lease through send_message too: an expected-generation check
    // must not be invalidated between the check and the frame write.
    std::unique_lock<std::timed_mutex> connection_lock(connection_mutex(), std::defer_lock);
    while (!connection_lock.try_lock_for(std::chrono::milliseconds(100))) {
      if (cancelled && cancelled()) {
        return false;
      }
    }
    if (cancelled && cancelled()) {
      return false;
    }
    if (session_singleton() != session ||
        !session_is_current(session) ||
        (expected_apply_generation &&
         apply_wait_generation().load(std::memory_order_acquire) != *expected_apply_generation)) {
      return false;
    }

    const bool reserve_untagged = begins_untagged_response &&
                                   session->protocol.load(std::memory_order_acquire) == ApplyResponseProtocol::Unknown;
    if (reserve_untagged) {
      // Set this before the write while the control/send ordering lease is
      // held; a successor cannot slip between the frame and this marker.
      session->untagged_response_pending.store(true, std::memory_order_release);
    }
    const bool sent = send_message(*session->pipe, type, payload, send_timeout_override_ms);
    if (!sent && reserve_untagged) {
      session->untagged_response_pending.store(false, std::memory_order_release);
    }
    if (sent && connection_generation_out) {
      *connection_generation_out = session->generation;
    }
    return sent;
  }

  static bool send_serialized_within(
    const SessionPtr &session,
    MsgType type,
    const std::vector<uint8_t> &payload,
    const std::chrono::steady_clock::time_point &deadline,
    std::optional<std::uint64_t> expected_apply_generation = std::nullopt,
    std::optional<int> send_timeout_cap_ms = std::nullopt,
    std::uint64_t *connection_generation_out = nullptr,
    bool begins_untagged_response = false,
    const std::function<bool()> &cancelled = {}
  ) {
    if (!session || !session->pipe) {
      return false;
    }

    std::unique_lock<std::timed_mutex> write_lock(write_mutex(), std::defer_lock);
    while (true) {
      if (cancelled && cancelled()) {
        return false;
      }
      const int remaining = remaining_timeout_ms(deadline);
      if (remaining <= 0) {
        return false;
      }
      if (write_lock.try_lock_for(std::chrono::milliseconds(std::min(remaining, 100)))) {
        break;
      }
    }

    std::unique_lock<std::timed_mutex> connection_lock(connection_mutex(), std::defer_lock);
    while (true) {
      if (cancelled && cancelled()) {
        return false;
      }
      const int remaining = remaining_timeout_ms(deadline);
      if (remaining <= 0) {
        return false;
      }
      if (connection_lock.try_lock_for(std::chrono::milliseconds(std::min(remaining, 100)))) {
        break;
      }
    }
    if (cancelled && cancelled()) {
      return false;
    }
    if (session_singleton() != session ||
        !session_is_current(session) ||
        (expected_apply_generation &&
         apply_wait_generation().load(std::memory_order_acquire) != *expected_apply_generation)) {
      return false;
    }

    const bool reserve_untagged = begins_untagged_response &&
                                  session->protocol.load(std::memory_order_acquire) == ApplyResponseProtocol::Unknown;
    if (reserve_untagged) {
      session->untagged_response_pending.store(true, std::memory_order_release);
    }
    int send_timeout_ms = remaining_timeout_ms(deadline);
    if (send_timeout_cap_ms) {
      send_timeout_ms = std::min(send_timeout_ms, *send_timeout_cap_ms);
    }
    const bool sent =
      send_timeout_ms > 0 &&
      send_message(*session->pipe, type, payload, send_timeout_ms);
    if (!sent && reserve_untagged) {
      session->untagged_response_pending.store(false, std::memory_order_release);
    }
    if (sent && connection_generation_out) {
      *connection_generation_out = session->generation;
    }
    return sent;
  }

  static void drop_connection_if_current(const SessionPtr &session, const char *reason) {
    SessionPtr retired;
    {
      std::lock_guard<std::timed_mutex> write_lock(write_mutex());
      std::lock_guard<std::timed_mutex> connection_lock(connection_mutex());
      if (session_singleton() == session) {
        BOOST_LOG(warning) << "Display helper IPC: dropping cached connection after " << reason;
        retired = retire_session_locked();
      }
    }
    if (retired) {
      // Do not call disconnect while another thread can be inside FramedPipe's
      // receive buffer. Old readers own their session and observe retirement
      // via the generation atomically before their next receive slice.
      reset_log_level_cache();
    }
  }

  static void drop_connection_if_current_within(
    const SessionPtr &session,
    const char *reason,
    const std::chrono::steady_clock::time_point &deadline
  ) {
    SessionPtr retired;
    {
      int remaining = remaining_timeout_ms(deadline);
      if (remaining <= 0) {
        return;
      }
      std::unique_lock<std::timed_mutex> write_lock(write_mutex(), std::defer_lock);
      if (!write_lock.try_lock_for(std::chrono::milliseconds(remaining))) {
        return;
      }
      remaining = remaining_timeout_ms(deadline);
      if (remaining <= 0) {
        return;
      }
      std::unique_lock<std::timed_mutex> connection_lock(connection_mutex(), std::defer_lock);
      if (!connection_lock.try_lock_for(std::chrono::milliseconds(remaining))) {
        return;
      }
      if (session_singleton() == session) {
        BOOST_LOG(warning) << "Display helper IPC: dropping cached connection after " << reason;
        retired = retire_session_locked();
      }
    }
    if (retired) {
      reset_log_level_cache();
    }
  }

  bool reset_connection_cancellable(
    std::function<bool()> cancellation_predicate,
    const std::chrono::steady_clock::time_point operation_deadline) {
    SessionPtr retired;
    {
      // The wait-generation change and retirement are one ordered control
      // decision. A fresh APPLY therefore either wins before this reset starts
      // or sees a new/no session; it cannot write to a pipe about to retire.
      std::unique_lock<std::timed_mutex> write_lock(write_mutex(), std::defer_lock);
      while (true) {
        if (cancellation_predicate && cancellation_predicate()) {
          return false;
        }
        const int remaining = remaining_timeout_ms(operation_deadline);
        if (remaining <= 0) {
          return false;
        }
        if (write_lock.try_lock_for(std::chrono::milliseconds(std::min(remaining, 100)))) {
          break;
        }
      }
      if ((cancellation_predicate && cancellation_predicate()) ||
          remaining_timeout_ms(operation_deadline) <= 0) {
        return false;
      }
      std::unique_lock<std::timed_mutex> connection_lock(connection_mutex(), std::defer_lock);
      while (true) {
        if (cancellation_predicate && cancellation_predicate()) {
          return false;
        }
        const int remaining = remaining_timeout_ms(operation_deadline);
        if (remaining <= 0) {
          return false;
        }
        if (connection_lock.try_lock_for(std::chrono::milliseconds(std::min(remaining, 100)))) {
          break;
        }
      }
      if ((cancellation_predicate && cancellation_predicate()) ||
          remaining_timeout_ms(operation_deadline) <= 0) {
        return false;
      }
      (void) apply_wait_generation().fetch_add(1, std::memory_order_acq_rel);
      if (session_singleton()) {
        BOOST_LOG(debug) << "Display helper IPC: resetting cached connection";
        retired = retire_session_locked();
      }
    }
    if (retired) {
      reset_log_level_cache();
    }
    return true;
  }

  void reset_connection() {
    (void) reset_connection_cancellable({});
  }
  std::optional<bool> wait_for_verification_result(
    int timeout_ms,
    std::function<bool()> cancellation_predicate,
    std::uint64_t expected_request_id,
    std::uint64_t expected_wait_generation,
    std::uint64_t expected_connection_generation) {
    if (expected_request_id == 0 || expected_wait_generation == 0 || expected_connection_generation == 0) {
      return std::nullopt;
    }
    if (apply_wait_generation().load(std::memory_order_acquire) != expected_wait_generation ||
        connection_generation().load(std::memory_order_acquire) != expected_connection_generation) {
      return std::nullopt;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    const auto cancelled = [
      expected_wait_generation,
      expected_connection_generation,
      cancellation_predicate = std::move(cancellation_predicate)
    ] {
      return (cancellation_predicate && cancellation_predicate()) ||
             apply_wait_generation().load(std::memory_order_acquire) != expected_wait_generation ||
             connection_generation().load(std::memory_order_acquire) != expected_connection_generation;
    };

    SessionPtr session;
    while (!cancelled()) {
      const int remaining = remaining_timeout_ms(deadline);
      if (remaining <= 0) {
        return std::nullopt;
      }
      std::unique_lock<std::timed_mutex> connection_lock(connection_mutex(), std::defer_lock);
      if (!connection_lock.try_lock_for(std::chrono::milliseconds(std::min(remaining, 100)))) {
        continue;
      }
      if (connection_generation().load(std::memory_order_acquire) == expected_connection_generation) {
        session = session_singleton();
      }
      break;
    }
    if (!session || !session_is_current(session) || cancelled()) {
      BOOST_LOG(debug) << "Display helper IPC: verification wait aborted - connection changed or request superseded.";
      return std::nullopt;
    }

    std::unique_lock<std::timed_mutex> response_lock;
    if (!lock_response_reader_until(session, response_lock, deadline, cancelled)) {
      return std::nullopt;
    }
    return wait_for_verification_result_locked(
      session,
      remaining_timeout_ms(deadline),
      expected_request_id,
      [session, cancelled] {
        return cancelled() || !session_is_current(session);
      });
  }

  bool send_log_level(int min_log_level) {
    const int clamped = std::clamp(min_log_level, 0, 6);
    {
      std::lock_guard<std::mutex> lock(log_level_mutex());
      const auto &last_level = last_log_level_sent();
      if (last_level && *last_level == clamped) {
        return true;
      }
    }

    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(clamped));
    const auto session = connected_session();
    if (send_serialized(session, MsgType::LogLevel, payload)) {
      std::lock_guard<std::mutex> lock(log_level_mutex());
      last_log_level_sent() = clamped;
      return true;
    }
    return false;
  }

  bool uses_v2_response_protocol() {
    return current_apply_response_protocol(current_session()) == ApplyResponseProtocol::V2;
  }

  bool send_apply_json(
    const std::string &json,
    std::uint64_t *request_id_out,
    std::uint64_t *wait_generation_out,
    std::uint64_t *connection_generation_out,
    std::function<bool()> cancellation_predicate,
    int operation_timeout_ms,
    bool shutdown_class_caller) {
    BOOST_LOG(debug) << "Display helper IPC: APPLY request queued (json_len=" << json.size() << ")";
    const bool operation_is_bounded = operation_timeout_ms > 0;
    const auto operation_deadline =
      operation_is_bounded ?
        std::chrono::steady_clock::now() + std::chrono::milliseconds(operation_timeout_ms) :
        std::chrono::steady_clock::time_point::max();
    const auto operation_cancelled = [&cancellation_predicate, operation_is_bounded, operation_deadline] {
      return (cancellation_predicate && cancellation_predicate()) ||
             (operation_is_bounded && std::chrono::steady_clock::now() >= operation_deadline);
    };
    if (request_id_out) {
      *request_id_out = 0;
    }
    if (wait_generation_out) {
      *wait_generation_out = 0;
    }
    if (connection_generation_out) {
      *connection_generation_out = 0;
    }
    if (operation_cancelled()) {
      return false;
    }
    const auto wait_generation =
      operation_is_bounded ?
        cancel_or_begin_apply_wait_within(
          operation_deadline,
          operation_cancelled
        ) :
        cancel_or_begin_apply_wait_cancellable(operation_cancelled);
    if (!wait_generation) {
      return false;
    }
    const auto request_id = next_apply_request_id().fetch_add(1, std::memory_order_relaxed);

    std::vector<uint8_t> payload;
    try {
      // The token is deliberately backward-compatible: v2 echoes it in the
      // ApplyResult and supplies a later verification acknowledgement, while
      // the legacy helper removes/ignores this metadata and returns its normal
      // untagged synchronous acknowledgement.  The response, not today's
      // configured helper preference, selects the live wire protocol.
      auto apply_json = nlohmann::json::parse(json, nullptr, false);
      if (!apply_json.is_object()) {
        BOOST_LOG(error) << "Display helper IPC: APPLY payload is not a JSON object.";
        return false;
      }
      apply_json["sunshine_apply_id"] = request_id;
      const auto serialized = apply_json.dump();
      payload.assign(serialized.begin(), serialized.end());
    } catch (...) {
      BOOST_LOG(error) << "Display helper IPC: failed to add APPLY request token to JSON payload.";
      return false;
    }

    if (operation_is_bounded && remaining_timeout_ms(operation_deadline) <= 0) {
      return false;
    }

    // Hold this session's response reader before sending APPLY. A superseded
    // v2 verification wait releases it in short slices; a known legacy helper
    // deliberately keeps its untagged lane serial until the old reply arrives.
    // Every APPLY now arrives with a synthesized deadline predicate, so the
    // predicate can no longer stand in for "this caller is shutting down": only
    // an explicit shutdown-class caller may collapse to the short IPC caps.
    const int connect_timeout_cap_ms =
      shutdown_class_caller ? kShutdownIpcTimeoutMs : kConnectTimeoutMs;
    const auto session =
      operation_is_bounded ?
        connected_session_within(
          operation_deadline,
          connect_timeout_cap_ms,
          operation_cancelled
        ) :
        connected_session(
          connect_timeout_cap_ms,
          operation_cancelled
        );
    if (!session) {
      BOOST_LOG(warning) << "Display helper IPC: APPLY aborted - no connection";
      return false;
    }
    const auto protocol = current_apply_response_protocol(session);
    const int protocol_timeout_ms = protocol == ApplyResponseProtocol::Legacy ?
                                      kLegacyApplyResultTimeoutMs :
                                      kV2ApplyResultTimeoutMs;
    const auto protocol_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(protocol_timeout_ms);
    const auto reader_deadline = std::min(operation_deadline, protocol_deadline);
    const auto response_cancelled = [&operation_cancelled, reader_deadline] {
      return operation_cancelled() || std::chrono::steady_clock::now() >= reader_deadline;
    };
    std::unique_lock<std::timed_mutex> response_lock;
    if (!lock_response_reader_until(
          session,
          response_lock,
          reader_deadline,
          [session, wait_generation = *wait_generation, response_cancelled] {
            return response_cancelled() ||
                   !session_is_current(session) ||
                   apply_wait_generation().load(std::memory_order_acquire) != wait_generation;
          })) {
      return false;
    }
    const int send_cap = shutdown_class_caller ? kShutdownIpcTimeoutMs : kSendTimeoutMs;
    std::uint64_t sent_connection_generation = 0;
    if (!send_serialized_within(
          session,
          MsgType::Apply,
          payload,
          reader_deadline,
          *wait_generation,
          send_cap,
          &sent_connection_generation,
          true,
          response_cancelled)) {
      return false;
    }
    remember_issued_apply_request_id(request_id);
    if (wait_generation_out) {
      *wait_generation_out = *wait_generation;
    }
    if (connection_generation_out) {
      *connection_generation_out = sent_connection_generation;
    }

    const int remaining_response_timeout_ms = remaining_timeout_ms(reader_deadline);
    if (remaining_response_timeout_ms > 0) {
      if (auto result = wait_for_apply_result_locked(
            session,
            request_id,
            protocol,
            remaining_response_timeout_ms,
            [session, wait_generation = *wait_generation, protocol, response_cancelled] {
              return response_cancelled() ||
                     !session_is_current(session) ||
                     (protocol != ApplyResponseProtocol::Legacy &&
                      apply_wait_generation().load(std::memory_order_acquire) != wait_generation);
            }
          )) {
        if (result->success && request_id_out && result->protocol == ApplyResponseProtocol::V2) {
          *request_id_out = request_id;
        }
        return result->success;
      }
    }

    if (protocol != ApplyResponseProtocol::Legacy &&
        apply_wait_generation().load(std::memory_order_acquire) != *wait_generation) {
      BOOST_LOG(debug) << "Display helper IPC: APPLY result wait superseded by a newer control command.";
      return false;
    }
    if (operation_is_bounded) {
      // The operation deadline is what expired to get here, so retiring the
      // pipe needs its own budget; otherwise the stale connection survives and
      // a late queued ApplyResult is read as the next request's answer.
      drop_connection_if_current_within(
        session,
        "missing APPLY result",
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kStaleConnectionRetireTimeoutMs)
      );
    } else {
      drop_connection_if_current(session, "missing APPLY result");
    }
    return false;
  }

  bool send_refresh_rate(const std::string &device_id, std::uint32_t numerator, std::uint32_t denominator) {
    if (device_id.empty() || numerator == 0 || denominator == 0) {
      return false;
    }

    const auto wait_generation = apply_wait_generation().load(std::memory_order_acquire);
    const auto session = connected_session(kRefreshRateConnectTimeoutMs);
    if (!session) {
      BOOST_LOG(warning) << "Display helper IPC: refresh-rate request aborted - no connection";
      return false;
    }
    const auto protocol = current_apply_response_protocol(session);
    const bool v2 = protocol == ApplyResponseProtocol::V2;
    const std::optional<std::uint64_t> request_id = v2 ?
      std::optional<std::uint64_t> {next_auxiliary_request_id().fetch_add(1, std::memory_order_relaxed)} :
      std::nullopt;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRefreshRateResultTimeoutMs);
    const auto cancelled = [session, wait_generation, v2] {
      return !session_is_current(session) ||
             (v2 && apply_wait_generation().load(std::memory_order_acquire) != wait_generation);
    };
    std::unique_lock<std::timed_mutex> response_lock;
    if (!lock_response_reader_until(session, response_lock, deadline, cancelled)) {
      return false;
    }

    std::vector<uint8_t> payload;
    payload.reserve((v2 ? 20 : 8) + device_id.size());
    if (v2) {
      append_u32_le(payload, kV2RefreshRateMagic);
    }
    append_u32_le(payload, numerator);
    append_u32_le(payload, denominator);
    if (request_id) {
      append_u64_le(payload, *request_id);
    }
    payload.insert(payload.end(), device_id.begin(), device_id.end());

    if (!send_serialized(
          session,
          MsgType::RefreshRate,
          payload,
          std::nullopt,
          wait_generation,
          nullptr,
          !v2)) {
      return false;
    }
    if (auto result = wait_for_refresh_rate_result_locked(
          session,
          request_id,
          cancelled
        )) {
      session->untagged_response_pending.store(false, std::memory_order_release);
      return *result;
    }

    if (cancelled()) {
      BOOST_LOG(debug) << "Display helper IPC: refresh-rate wait superseded by a display control command.";
      return false;
    }

    drop_connection_if_current(session, "missing refresh-rate result");
    return false;
  }

  bool send_revert(const std::string &json_payload) {
    BOOST_LOG(debug) << "Display helper IPC: REVERT request queued";
    const auto wait_generation = cancel_or_begin_apply_wait();
    const auto session = connected_session();
    if (!session) {
      BOOST_LOG(warning) << "Display helper IPC: REVERT aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload(json_payload.begin(), json_payload.end());
    return send_serialized(session, MsgType::Revert, payload, std::nullopt, wait_generation);
  }

  bool send_revert_within(
    const std::string &json_payload,
    const std::chrono::steady_clock::time_point operation_deadline,
    std::function<bool()> cancellation_predicate) {
    BOOST_LOG(debug) << "Display helper IPC: bounded REVERT request queued";
    const auto operation_cancelled = [&] {
      return (cancellation_predicate && cancellation_predicate()) ||
             std::chrono::steady_clock::now() >= operation_deadline;
    };
    if (operation_cancelled()) {
      return false;
    }

    const auto wait_generation = cancel_or_begin_apply_wait_within(
      operation_deadline,
      operation_cancelled
    );
    if (!wait_generation) {
      return false;
    }
    const auto session = connected_session_within(
      operation_deadline,
      kConnectTimeoutMs,
      operation_cancelled
    );
    if (!session) {
      BOOST_LOG(warning) << "Display helper IPC: bounded REVERT aborted - no connection";
      return false;
    }

    std::vector<uint8_t> payload(json_payload.begin(), json_payload.end());
    return send_serialized_within(
      session,
      MsgType::Revert,
      payload,
      operation_deadline,
      *wait_generation,
      kSendTimeoutMs,
      nullptr,
      false,
      operation_cancelled
    );
  }

  bool send_export_golden(const std::string &json_payload) {
    BOOST_LOG(debug) << "Display helper IPC: EXPORT_GOLDEN request queued";
    const auto wait_generation = apply_wait_generation().load(std::memory_order_acquire);
    const auto session = connected_session();
    if (!session) {
      BOOST_LOG(warning) << "Display helper IPC: EXPORT_GOLDEN aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload(json_payload.begin(), json_payload.end());
    return send_serialized(session, MsgType::ExportGolden, payload, std::nullopt, wait_generation);
  }

  bool send_reset() {
    BOOST_LOG(debug) << "Display helper IPC: RESET request queued";
    const auto wait_generation = cancel_or_begin_apply_wait();
    const auto session = connected_session();
    if (!session) {
      BOOST_LOG(warning) << "Display helper IPC: RESET aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload;
    return send_serialized(session, MsgType::Reset, payload, std::nullopt, wait_generation);
  }

  bool send_disarm_restore() {
    BOOST_LOG(info) << "Display helper IPC: DISARM request queued";
    const auto wait_generation = cancel_or_begin_apply_wait();
    const auto session = connected_session();
    if (!session) {
      BOOST_LOG(warning) << "Display helper IPC: DISARM aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload;
    return send_serialized(session, MsgType::Disarm, payload, std::nullopt, wait_generation);
  }

  bool send_disarm_restore_fast(
    int timeout_ms,
    const std::chrono::steady_clock::time_point operation_deadline) {
    BOOST_LOG(debug) << "Display helper IPC: DISARM (fast) request queued (timeout_ms=" << timeout_ms << ")";
    if (timeout_ms <= 0) {
      return false;
    }
    const auto deadline = std::min(
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms),
      operation_deadline
    );
    const auto wait_generation = cancel_or_begin_apply_wait_within(deadline);
    if (!wait_generation) {
      return false;
    }
    const auto session = cached_connected_session_within(deadline);
    // Stream-start DISARM is authoritative. A prior restore may already have
    // changed topology and become unconfirmed, but it must not continue racing
    // the next session's virtual-display initialization.
    std::vector<uint8_t> payload {1u};
    return send_serialized_within(session, MsgType::Disarm, payload, deadline, *wait_generation);
  }

  bool send_snapshot_current(const std::string &json_payload) {
    BOOST_LOG(debug) << "Display helper IPC: SNAPSHOT_CURRENT request queued";
    const auto wait_generation = apply_wait_generation().load(std::memory_order_acquire);
    const auto session = connected_session();
    if (!session) {
      BOOST_LOG(warning) << "Display helper IPC: SNAPSHOT_CURRENT aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload(json_payload.begin(), json_payload.end());
    return send_serialized(session, MsgType::SnapshotCurrent, payload, std::nullopt, wait_generation);
  }

  bool send_snapshot_current_and_wait(const std::string &json_payload, int timeout_ms) {
    BOOST_LOG(debug) << "Display helper IPC: SNAPSHOT_CURRENT request queued with completion wait";
    if (timeout_ms <= 0) {
      return false;
    }
    const auto wait_generation = apply_wait_generation().load(std::memory_order_acquire);
    const auto session = connected_session();
    if (!session || current_apply_response_protocol(session) != ApplyResponseProtocol::V2) {
      BOOST_LOG(warning) << "Display helper IPC: SNAPSHOT_CURRENT completion wait requires a confirmed v2 helper.";
      return false;
    }
    const auto request_id = next_auxiliary_request_id().fetch_add(1, std::memory_order_relaxed);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    const auto cancelled = [session, wait_generation] {
      return !session_is_current(session) ||
             apply_wait_generation().load(std::memory_order_acquire) != wait_generation;
    };
    std::unique_lock<std::timed_mutex> response_lock;
    if (!lock_response_reader_until(session, response_lock, deadline, cancelled)) {
      return false;
    }

    std::vector<uint8_t> payload;
    try {
      auto snapshot_json = nlohmann::json::parse(json_payload.empty() ? "{}" : json_payload, nullptr, false);
      if (snapshot_json.is_array()) {
        // The long-standing snapshot API accepts a bare exclusion array. Keep
        // that wire shape at the public boundary while adding the v2 token in
        // an object the helper can distinguish from metadata-only requests.
        snapshot_json = nlohmann::json {
          {"exclude_devices", std::move(snapshot_json)},
        };
      }
      if (!snapshot_json.is_object()) {
        BOOST_LOG(error) << "Display helper IPC: SNAPSHOT_CURRENT payload must be a JSON object or exclusion array.";
        return false;
      }
      snapshot_json["sunshine_snapshot_id"] = request_id;
      const auto serialized = snapshot_json.dump();
      payload.assign(serialized.begin(), serialized.end());
    } catch (...) {
      BOOST_LOG(error) << "Display helper IPC: failed to add SNAPSHOT_CURRENT request token.";
      return false;
    }
    if (!send_serialized(session, MsgType::SnapshotCurrent, payload, std::nullopt, wait_generation)) {
      return false;
    }
    if (auto result = wait_for_snapshot_result_locked(
          session,
          remaining_timeout_ms(deadline),
          request_id,
          cancelled
        )) {
      return *result;
    }

    if (cancelled()) {
      BOOST_LOG(debug) << "Display helper IPC: snapshot wait superseded by a display control command.";
      return false;
    }

    drop_connection_if_current(session, "missing SNAPSHOT_CURRENT result");
    return false;
  }

  bool send_snapshot_current_within(
    const std::string &json_payload,
    const std::chrono::steady_clock::time_point operation_deadline,
    std::function<bool()> cancellation_predicate) {
    BOOST_LOG(debug) << "Display helper IPC: bounded SNAPSHOT_CURRENT request queued";
    // The snapshot shares the caller's stream-start budget while holding the
    // single response reader, so it keeps the same per-stage cap the unbounded
    // path uses. Without it a slow snapshot starves the APPLY that follows.
    const auto stage_deadline = std::min(
      operation_deadline,
      std::chrono::steady_clock::now() + std::chrono::milliseconds(kSnapshotResultTimeoutMs)
    );
    const auto operation_cancelled = [&] {
      return (cancellation_predicate && cancellation_predicate()) ||
             std::chrono::steady_clock::now() >= stage_deadline;
    };
    if (operation_cancelled()) {
      return false;
    }

    const auto wait_generation = apply_wait_generation().load(std::memory_order_acquire);
    const auto session = connected_session_within(
      stage_deadline,
      kConnectTimeoutMs,
      operation_cancelled
    );
    if (!session) {
      BOOST_LOG(warning) << "Display helper IPC: bounded SNAPSHOT_CURRENT aborted - no connection";
      return false;
    }
    const auto protocol = current_apply_response_protocol(session);
    const auto cancelled = [session, wait_generation, operation_cancelled] {
      return operation_cancelled() ||
             !session_is_current(session) ||
             apply_wait_generation().load(std::memory_order_acquire) != wait_generation;
    };

    std::vector<uint8_t> payload;
    std::optional<std::uint64_t> request_id;
    if (protocol == ApplyResponseProtocol::V2) {
      request_id = next_auxiliary_request_id().fetch_add(1, std::memory_order_relaxed);
      try {
        auto snapshot_json = nlohmann::json::parse(
          json_payload.empty() ? "{}" : json_payload,
          nullptr,
          false
        );
        if (snapshot_json.is_array()) {
          snapshot_json = nlohmann::json {
            {"exclude_devices", std::move(snapshot_json)},
          };
        }
        if (!snapshot_json.is_object()) {
          BOOST_LOG(error) << "Display helper IPC: SNAPSHOT_CURRENT payload must be a JSON object or exclusion array.";
          return false;
        }
        snapshot_json["sunshine_snapshot_id"] = *request_id;
        const auto serialized = snapshot_json.dump();
        payload.assign(serialized.begin(), serialized.end());
      } catch (...) {
        BOOST_LOG(error) << "Display helper IPC: failed to add bounded SNAPSHOT_CURRENT request token.";
        return false;
      }
    } else {
      payload.assign(json_payload.begin(), json_payload.end());
    }

    std::unique_lock<std::timed_mutex> response_lock;
    if (request_id &&
        !lock_response_reader_until(
          session,
          response_lock,
          stage_deadline,
          cancelled)) {
      return false;
    }
    if (!send_serialized_within(
          session,
          MsgType::SnapshotCurrent,
          payload,
          stage_deadline,
          wait_generation,
          kSendTimeoutMs,
          nullptr,
          false,
          cancelled)) {
      return false;
    }
    if (!request_id) {
      return true;
    }

    const int remaining = remaining_timeout_ms(stage_deadline);
    if (remaining > 0) {
      if (auto result = wait_for_snapshot_result_locked(
            session,
            remaining,
            *request_id,
            cancelled
          )) {
        return *result;
      }
    }
    // Budget exhaustion is the normal way to reach this point, so it must not
    // suppress retiring the pipe; only a genuine supersession leaves it usable.
    const bool superseded = (cancellation_predicate && cancellation_predicate()) ||
                            !session_is_current(session) ||
                            apply_wait_generation().load(std::memory_order_acquire) != wait_generation;
    if (!superseded) {
      drop_connection_if_current_within(
        session,
        "missing bounded SNAPSHOT_CURRENT result",
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kStaleConnectionRetireTimeoutMs)
      );
    }
    return false;
  }

  bool send_stop() {
    BOOST_LOG(info) << "Display helper IPC: STOP request queued";
    (void) cancel_or_begin_apply_wait();
    const auto session = connected_session();
    if (!session) {
      BOOST_LOG(warning) << "Display helper IPC: STOP aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload;
    return send_serialized(session, MsgType::Stop, payload);
  }

  bool send_ping() {
    // No logging for ping path to reduce log spam
    const auto session = connected_session();
    std::vector<uint8_t> payload;
    return send_serialized(session, MsgType::Ping, payload);
  }

  bool send_ping_cancellable(
    int timeout_ms,
    std::function<bool()> cancellation_predicate,
    const std::chrono::steady_clock::time_point operation_deadline) {
    if (timeout_ms <= 0 || (cancellation_predicate && cancellation_predicate())) {
      return false;
    }
    const auto deadline = std::min(
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms),
      operation_deadline
    );
    const auto session = connected_session_within(
      deadline,
      timeout_ms,
      cancellation_predicate
    );
    std::vector<uint8_t> payload;
    return send_serialized_within(
      session,
      MsgType::Ping,
      payload,
      deadline,
      std::nullopt,
      timeout_ms,
      nullptr,
      false,
      cancellation_predicate);
  }

  bool send_ping_fast(
    int timeout_ms,
    const std::chrono::steady_clock::time_point operation_deadline) {
    if (timeout_ms <= 0) {
      return false;
    }
    const auto deadline = std::min(
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms),
      operation_deadline
    );
    const auto session = cached_connected_session_within(deadline);
    std::vector<uint8_t> payload;
    return send_serialized_within(session, MsgType::Ping, payload, deadline);
  }
}  // namespace platf::display_helper_client

#endif
