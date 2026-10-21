#pragma once

#include "src/platform/windows/display_helper_borrow_gate.h"

#include <map>
#include <deque>
#include <variant>

namespace display_helper::borrow {
  // Actual producer used by the v2 pipe parser/queue. Native process liveness
  // and the quiesced SudoVDA handle are injected leaves; none of the protocol,
  // token, admission, draining or stale-release decisions are test substitutes.
  class control_t {
  public:
    using alive_t = std::function<std::optional<bool>(const process_context_t &)>;
    control_t(gate_t &gate, process_context_t helper, alive_t alive,
              std::function<std::uint64_t()> keepalive)
      : gate_(gate), helper_(helper), alive_(std::move(alive)), keepalive_(std::move(keepalive)) {}

    result_t acquire(const request_t &request, std::uint64_t connection_epoch,
                     std::chrono::steady_clock::time_point deadline) {
      result_t result {{request, helper_, connection_epoch}, false};
      if (!helper_.valid() || connection_epoch == 0 || !request.valid()) return result;
      const auto alive = alive_(request.controller);
      if (!alive || !*alive) return result;
      const auto existing = tokens_.find(request);
      if (existing != tokens_.end()) {
        // Retransmission/reconnect retains the original exact token. A later
        // retired request from this controller cannot invalidate a live hold.
        result.token = existing->second;
      } else {
        if (!gate_.acquire(request, deadline, [&] {
              const auto current = alive_(request.controller);
              return !current || !*current;
            })) return result;
        tokens_.emplace(request, result.token);
      }
      result.native_keepalive_handle = keepalive_();
      result.success = true;
      return result;
    }

    result_t release(const token_t &token, std::uint64_t incoming_epoch) {
      result_t result {token, false};
      const auto existing = tokens_.find(token.request);
      const bool cancellation = !token.helper.valid() && token.connection_epoch == 0;
      if (cancellation) {
        // Unacknowledged requests are cancelled only on their originating
        // pipe. They cannot clear a newer connection's acknowledged token.
        if (existing != tokens_.end() && existing->second.connection_epoch != incoming_epoch) return result;
        gate_.release(token.request);
        if (existing != tokens_.end()) {
          result.token = existing->second;
          tokens_.erase(existing);
        } else result.token = {token.request, helper_, incoming_epoch};
        result.success = true;
        return result;
      }
      if (token.helper != helper_ || existing == tokens_.end() || existing->second != token) return result;
      gate_.release(token.request);
      tokens_.erase(existing);
      result.success = true;
      return result;
    }

    void reap_dead_controllers() {
      for (auto it = tokens_.begin(); it != tokens_.end();) {
        const auto alive = alive_(it->first.controller);
        if (alive && !*alive) { gate_.release(it->first); it = tokens_.erase(it); }
        else ++it;  // access/query uncertainty retains inhibition
      }
    }

    bool inhibited() const { return gate_.inhibited(); }
  private:
    gate_t &gate_;
    process_context_t helper_;
    alive_t alive_;
    std::function<std::uint64_t()> keepalive_;
    std::map<request_t, token_t> tokens_;
  };

  class mailbox_t {
  public:
    explicit mailbox_t(control_t &control) : control_(control) {}
    bool post(std::uint8_t type, std::span<const std::uint8_t> payload, std::uint64_t epoch,
              std::optional<std::uint32_t> peer_pid) {
      if (epoch == 0) return false;
      std::optional<command_t> command;
      if (type == acquire_message) {
        if (auto request = decode_acquire(payload); request && peer_pid && *peer_pid == request->controller.pid)
          command = command_t {*request, epoch};
      } else if (type == release_message) {
        if (auto token = decode_release(payload); token && peer_pid && *peer_pid == token->request.controller.pid)
          command = command_t {*token, epoch};
      }
      if (!command) return false;
      std::lock_guard lock(mutex_);
      queue_.push_back(std::move(*command));
      return true;
    }
    bool service(const std::function<void(std::uint8_t, const result_t &, std::uint64_t)> &reply) {
      std::optional<command_t> command;
      { std::lock_guard lock(mutex_); if (!queue_.empty()) { command = std::move(queue_.front()); queue_.pop_front(); } }
      if (command) {
        if (const auto *request = std::get_if<request_t>(&command->body))
          reply(acquire_result_message, control_.acquire(*request, command->epoch,
            std::chrono::steady_clock::now() + std::chrono::seconds(1)), command->epoch);
        else reply(release_result_message, control_.release(std::get<token_t>(command->body), command->epoch), command->epoch);
      }
      control_.reap_dead_controllers();
      return command.has_value();
    }
    bool inhibited() const { return control_.inhibited(); }
  private:
    struct command_t { std::variant<request_t, token_t> body; std::uint64_t epoch; };
    control_t &control_;
    std::mutex mutex_;
    std::deque<command_t> queue_;
  };
}
