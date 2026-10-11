/**
 * @file src/platform/macos/display_policy.cpp
 * @brief Definitions for the CoreGraphics-independent display decisions.
 */
// standard includes
#include <algorithm>

// local includes
#include "display_policy.h"

namespace platf::macos_display_policy {
  namespace {
    bool contains(const std::vector<display_id_t> &ids, const display_id_t id) {
      return std::find(ids.begin(), ids.end(), id) != ids.end();
    }
  }  // namespace

  lid_close_action_e lid_close_action(const bool end_streams_on_lid_close, const std::size_t sessions, const bool app_running) {
    if (!end_streams_on_lid_close || sessions == 0) {
      return lid_close_action_e::none;
    }
    return app_running ? lid_close_action_e::quit_app : lid_close_action_e::end_streams;
  }

  exclusive_plan_t plan_exclusive_layout(const std::vector<display_id_t> &active, const std::vector<display_id_t> &keep_on, const std::vector<display_id_t> &tracked) {
    exclusive_plan_t plan;
    for (const auto id : active) {
      if (contains(keep_on, id) || contains(plan.turn_off, id)) {
        continue;
      }
      plan.turn_off.push_back(id);
      if (!contains(tracked, id)) {
        plan.newly_tracked.push_back(id);
      }
    }
    return plan;
  }

  attempt_budget_t::attempt_budget_t(const std::size_t limit, const clock::duration window):
      limit {limit},
      window {window} {
  }

  bool attempt_budget_t::try_spend(const clock::time_point now) {
    while (!attempts.empty() && now - attempts.front() >= window) {
      attempts.pop_front();
    }
    if (attempts.size() >= limit) {
      return false;
    }
    attempts.push_back(now);
    return true;
  }

  void attempt_budget_t::reset() {
    attempts.clear();
  }

  display_presence_t::display_presence_t(const clock::duration grace):
      grace {grace} {
  }

  bool display_presence_t::lost(const bool online, const clock::time_point now) {
    if (online) {
      offline_since.reset();
      return false;
    }
    if (!offline_since) {
      offline_since = now;
    }
    return now - *offline_since >= grace;
  }
}  // namespace platf::macos_display_policy
