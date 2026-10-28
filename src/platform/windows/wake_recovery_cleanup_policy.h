#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <utility>

namespace platf::wake_recovery_cleanup_policy {
  enum class helper_status_t : std::uint8_t {
    unknown,
    active,
    failed,
    restored,
  };

  /**
   * State for the narrow automatic cleanup that follows a failed desktop
   * restore. Target IDs are captured when the restore is requested and can
   * never be discovered or widened by a later wake/topology event.
   */
  class state_t {
  public:
    bool begin(std::uint64_t ticket, std::set<std::string> captured_targets) {
      if (ticket == 0 || ticket <= ticket_) return false;
      ticket_ = ticket;
      targets_ = std::move(captured_targets);
      status_ = helper_status_t::active;
      baseline_event_revision_ = 0;
      last_event_ = 0;
      attempted_targets_.clear();
      return true;
    }

    bool set_event_baseline(std::uint64_t ticket, std::uint64_t revision) {
      if (ticket == 0 || ticket != ticket_ || status_ != helper_status_t::active || last_event_ != 0) return false;
      baseline_event_revision_ = revision;
      return true;
    }

    bool update_status(std::uint64_t ticket, helper_status_t status) {
      if (ticket == 0 || ticket != ticket_) return false;
      if (status == helper_status_t::unknown) return false;
      // A completed result is terminal for this ticket. A later stale ACTIVE
      // sample may not roll it back into an unresolved state.
      if (status_ == helper_status_t::failed || status_ == helper_status_t::restored) return false;
      status_ = status;
      return true;
    }

    bool note_external_event(std::uint64_t ticket, std::uint64_t event_id, bool self_generated) {
      if (self_generated || ticket == 0 || ticket != ticket_ || event_id <= baseline_event_revision_ ||
          event_id <= last_event_ || status_ == helper_status_t::unknown ||
          status_ == helper_status_t::restored) {
        return false;
      }
      last_event_ = event_id;
      return true;
    }

    bool authorize_target(
      std::uint64_t ticket,
      std::uint64_t event_id,
      const std::string &target,
      bool physical_outputs_verified,
      bool owners_clear,
      bool target_still_managed
    ) const {
      return ticket != 0 && ticket == ticket_ && event_id != 0 && event_id == last_event_ &&
             status_ == helper_status_t::failed && physical_outputs_verified && owners_clear &&
             target_still_managed && targets_.contains(target) && !attempted_targets_.contains(target);
    }

    void record_attempt(std::uint64_t ticket, const std::string &target) {
      if (ticket == ticket_ && status_ == helper_status_t::failed && targets_.contains(target)) {
        attempted_targets_.insert(target);
      }
    }

    std::uint64_t ticket() const { return ticket_; }
    helper_status_t status() const { return status_; }
    bool captured_target(const std::string &target) const { return targets_.contains(target); }

  private:
    std::uint64_t ticket_ {0};
    std::uint64_t baseline_event_revision_ {0};
    std::uint64_t last_event_ {0};
    helper_status_t status_ {helper_status_t::unknown};
    std::set<std::string> targets_;
    std::set<std::string> attempted_targets_;
  };
}  // namespace platf::wake_recovery_cleanup_policy
