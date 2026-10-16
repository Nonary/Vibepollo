#pragma once

#include <cstdint>
#include <optional>

namespace display_helper::recovery_status {
  class monitor_power_edge_policy {
  public:
    void reset() {
      previous_on_.reset();
    }

    bool observe(bool supported_value, bool monitor_on) {
      if (!supported_value) {
        previous_on_.reset();
        return false;
      }
      const bool woke = previous_on_.has_value() && !*previous_on_ && monitor_on;
      previous_on_ = monitor_on;
      return woke;
    }

  private:
    std::optional<bool> previous_on_;
  };

  enum class status : std::uint8_t {
    unknown = 0,
    active = 1,
    failed = 2,
    restored = 3,
  };

  /// Shared, portable ticket policy used by both Windows helper engines.
  /// Callers serialize access and supply their live worker/queue state.
  class policy {
  public:
    void begin(std::uint64_t ticket, std::uint64_t generation, std::uint64_t epoch) {
      ticket_ = ticket;
      generation_ = generation;
      epoch_ = epoch;
      ticket_event_revision_base_ = event_revision_;
      status_ = ticket == 0 ? status::unknown : status::active;
      parked_ = false;
    }

    void restart(std::uint64_t generation, std::uint64_t epoch) {
      if (ticket_ == 0) return;
      generation_ = generation;
      epoch_ = epoch;
      status_ = status::active;
      parked_ = false;
    }

    void supersede() {
      ticket_ = 0;
      generation_ = 0;
      epoch_ = 0;
      status_ = status::unknown;
      parked_ = false;
    }

    std::uint64_t observe_event() {
      return ++event_revision_;
    }

    bool publish(status value, std::uint64_t ticket, std::uint64_t generation, std::uint64_t epoch) {
      if (ticket_ == 0 || ticket != ticket_ || generation != generation_ || epoch != epoch_) {
        return false;
      }
      status_ = value;
      return true;
    }

    status query(
      std::uint64_t requested_ticket,
      std::uint64_t requested_generation,
      std::uint64_t requested_epoch,
      std::uint64_t current_generation,
      std::uint64_t current_epoch,
      bool worker_active,
      bool operation_queued,
      bool park
    ) {
      if (ticket_ == 0 || requested_ticket != ticket_ || requested_generation != generation_ ||
          requested_epoch != epoch_ || current_generation != generation_ || current_epoch != epoch_) {
        return status::unknown;
      }
      if (worker_active || operation_queued) {
        return status::active;
      }
      if (status_ == status::failed && park) {
        parked_ = true;
      }
      return status_;
    }

    bool parked() const {
      return parked_;
    }

    std::uint64_t ticket() const {
      return ticket_;
    }

    std::uint64_t generation() const {
      return generation_;
    }

    std::uint64_t epoch() const {
      return epoch_;
    }

    status value() const {
      return status_;
    }

    std::uint64_t event_revision() const {
      return event_revision_ - ticket_event_revision_base_;
    }

  private:
    std::uint64_t ticket_ = 0;
    std::uint64_t generation_ = 0;
    std::uint64_t epoch_ = 0;
    status status_ = status::unknown;
    bool parked_ = false;
    std::uint64_t event_revision_ = 0;
    std::uint64_t ticket_event_revision_base_ = 0;
  };
}  // namespace display_helper::recovery_status
