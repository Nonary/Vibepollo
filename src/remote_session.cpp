#include "remote_session.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <mutex>
#include <unordered_set>

namespace remote_session {
  namespace {
    std::mutex monitor_runtime_hooks_mutex;
    monitor_runtime_hooks_t monitor_runtime_hooks;
    // Moonlight refreshes the app catalogue after the first synthetic launch
    // failure before the user can launch Terminate again. That round trip can
    // exceed ten seconds on mobile clients even when the second launch is
    // immediate from the user's perspective.
    constexpr auto terminate_confirmation_window = std::chrono::seconds {60};
    struct terminate_confirmation_t {
      std::uint64_t generation {};
      std::int32_t app_id {};
      std::chrono::steady_clock::time_point expires_at {};
    };
    std::mutex terminate_confirmation_mutex;
    std::unordered_map<std::string, terminate_confirmation_t> terminate_confirmations;
    bool equal_folded(std::string_view left, std::string_view right) {
      if (left.size() != right.size()) return false;
      for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) != std::tolower(static_cast<unsigned char>(right[i]))) return false;
      }
      return true;
    }
    bool contains(const std::vector<std::string> &values, std::string_view value) {
      return std::find(values.begin(), values.end(), value) != values.end();
    }
    bool owns_game(const caller_t &caller, const game_t &game) { return game.running && caller.paired && caller.uuid == game.owner_uuid; }
  }

  bool reserved_name(const std::string_view name) {
    return equal_folded(name, "Remote Input") || equal_folded(name, "Remote Monitor") || equal_folded(name, "Terminate");
  }

  control_e identify(const std::int32_t id, const std::string_view uuid) {
    if (id == resume_id || id == 2147483601 || uuid == "9a1c5a25-58fe-40e0-b9aa-7d3f00000001") return control_e::resume;
    if (id == disconnect_monitor_id || id == 2147483602 || uuid == "9a1c5a25-58fe-40e0-b9aa-7d3f00000002") return control_e::disconnect_monitor;
    if (id == disconnect_input_id || id == 2147483603 || uuid == "9a1c5a25-58fe-40e0-b9aa-7d3f00000003") return control_e::disconnect_input;
    if (id == terminate_id || id == 2147483604 || uuid == "9a1c5a25-58fe-40e0-b9aa-7d3f00000004") return control_e::terminate;
    if (id == monitor_id || id == 2147483605 || uuid == "9a1c5a25-58fe-40e0-b9aa-7d3f00000005") return control_e::monitor;
    if (id == input_id || id == 2147483606 || uuid == "9a1c5a25-58fe-40e0-b9aa-7d3f00000006") return control_e::input;
    return control_e::none;
  }

  std::string synthetic_uuid(const control_e control) {
    const auto suffix = static_cast<unsigned>(control);
    return suffix >= 1 && suffix <= 6 ? "9a1c5a25-58fe-40e0-b9aa-7d3f0000000" + std::to_string(suffix) : std::string {};
  }

  app_t synthetic(const control_e control) {
    switch (control) {
      case control_e::resume: return {resume_id, synthetic_uuid(control), "Resume", true};
      case control_e::disconnect_monitor: return {disconnect_monitor_id, synthetic_uuid(control), "Disconnect Monitor", true};
      case control_e::disconnect_input: return {disconnect_input_id, synthetic_uuid(control), "Disconnect Input", true};
      case control_e::terminate: return {terminate_id, synthetic_uuid(control), "Terminate", true};
      case control_e::monitor: return {monitor_id, synthetic_uuid(control), "Remote Monitor", true};
      case control_e::input: return {input_id, synthetic_uuid(control), "Remote Input", true};
      default: return {};
    }
  }

  std::optional<std::string_view> synthetic_artwork_filename(const control_e control) {
    switch (control) {
      case control_e::resume: return "resume.png";
      case control_e::disconnect_monitor: return "disconnect-remote-monitor.png";
      case control_e::disconnect_input: return "disconnect-remote-input.png";
      case control_e::terminate: return "terminate.png";
      case control_e::monitor: return "remote-monitor.png";
      case control_e::input: return "remote-input.png";
      default: return std::nullopt;
    }
  }

  projection_t project(const caller_t &caller, const game_t &game, const owner_t &owner, const std::vector<app_t> &configured) {
    projection_t result;
    std::vector<app_t> visible_configured;
    std::copy_if(configured.begin(), configured.end(), std::back_inserter(visible_configured), [](const app_t &app) {
      return !reserved_name(app.title);
    });
    if (owner.role == role_e::monitor) {
      result.catalogue = {synthetic(control_e::resume), synthetic(control_e::disconnect_monitor)};
      return result;
    }
    if (owner.role == role_e::input) {
      result.catalogue = {synthetic(control_e::disconnect_input)};
      return result;
    }
    if (owns_game(caller, game)) {
      result.free = false;
      result.current_game = game.app.id;
      result.catalogue = visible_configured;
      result.catalogue.push_back(synthetic(control_e::input));
      result.catalogue.push_back(synthetic(control_e::monitor));
      return result;
    }
    if (game.running) {
      result.catalogue = {synthetic(control_e::resume), synthetic(control_e::terminate), game.app, synthetic(control_e::input), synthetic(control_e::monitor)};
      return result;
    }
    result.catalogue = visible_configured;
    result.catalogue.push_back(synthetic(control_e::input));
    result.catalogue.push_back(synthetic(control_e::monitor));
    return result;
  }

  dispatch_t dispatch(const caller_t &caller, const game_t &game, const owner_t &owner, const control_e control) {
    dispatch_t result {.control = control};
    switch (control) {
      case control_e::resume:
      case control_e::running_game:
        result.permission = permission_e::view;
        result.allowed = caller.may_view && (game.running || owner.role == role_e::monitor);
        result.resume = result.allowed;
        if (result.allowed) {
          result.resume_role = owner.role == role_e::monitor ? role_e::monitor : role_e::game;
        }
        break;
      case control_e::input:
        result.permission = permission_e::launch;
        result.allowed = caller.may_launch && owner.role == role_e::none;
        break;
      case control_e::monitor:
        result.permission = permission_e::launch;
        // Moonlight may retry a slow /launch request or invoke a cached Remote
        // Monitor tile after ownership has already been published. For the
        // same paired client this is an idempotent activation retry, not a
        // permission failure. Another client's owner is never passed here.
        result.allowed = caller.may_launch && (owner.role == role_e::none || owner.role == role_e::monitor);
        result.resume = result.allowed && owner.role == role_e::monitor;
        if (result.resume) result.resume_role = role_e::monitor;
        break;
      case control_e::terminate:
        result.permission = permission_e::terminate;
        result.allowed = caller.may_terminate && game.running;
        result.terminate = result.allowed;
        break;
      case control_e::disconnect_monitor:
        result.permission = permission_e::terminate;
        result.allowed = caller.may_terminate && (owner.role == role_e::monitor || owner.role == role_e::none);
        result.already_complete = result.allowed && owner.role == role_e::none;
        break;
      case control_e::disconnect_input:
        result.permission = permission_e::terminate;
        result.allowed = caller.may_terminate && (owner.role == role_e::input || owner.role == role_e::none);
        result.already_complete = result.allowed && owner.role == role_e::none;
        break;
      default: break;
    }
    return result;
  }

  bool joins_existing_game_output(const role_e role, const bool stream_active) {
    return role == role_e::game && stream_active;
  }

  std::string_view stream_start_response_key(const bool launched_from_applist) {
    return launched_from_applist ? "gamesession" : "resume";
  }

  std::optional<control_completion_t> successful_control_completion(const control_e control) {
    // These entries are host controls presented through Moonlight's launch UI,
    // not streamable applications. Apollo established 410 as the deliberate
    // launch failure that completes the action and displays its message instead
    // of making Moonlight wait for a nonexistent RTSP session URL.
    switch (control) {
      case control_e::disconnect_monitor: return control_completion_t {410, "Remote Monitor disconnected successfully."};
      case control_e::disconnect_input: return control_completion_t {410, "Remote Input disconnected successfully."};
      case control_e::terminate: return control_completion_t {410, "Active stream terminated. Remote Monitor and Remote Input remain connected."};
      default: return std::nullopt;
    }
  }

  terminate_confirmation_e arm_or_confirm_termination(
    const std::string_view client_uuid,
    const std::uint64_t generation,
    const std::int32_t app_id,
    const std::chrono::steady_clock::time_point now
  ) {
    std::lock_guard lock {terminate_confirmation_mutex};
    const auto it = terminate_confirmations.find(std::string {client_uuid});
    if (it != terminate_confirmations.end()) {
      const auto pending = it->second;
      terminate_confirmations.erase(it);
      if (pending.generation == generation && pending.app_id == app_id && pending.expires_at > now) {
        return terminate_confirmation_e::confirmed;
      }
    }
    terminate_confirmations.insert_or_assign(
      std::string {client_uuid},
      terminate_confirmation_t {generation, app_id, now + terminate_confirmation_window}
    );
    return terminate_confirmation_e::prompt;
  }

  std::string_view termination_confirmation_message() {
    return "This will close the active stream but leave Remote Monitor and Remote Input connected. Launch Terminate again within 60 seconds to confirm this was intentional.";
  }

  void clear_termination_confirmation(const std::string_view client_uuid) {
    std::lock_guard lock {terminate_confirmation_mutex};
    terminate_confirmations.erase(std::string {client_uuid});
  }

  bool input_uses_display_or_audio(const role_e role) { return role != role_e::input; }

  capture_plan_t capture_plan(const role_e role, std::optional<std::string> output) {
    if (role == role_e::input) {
      return {.source = capture_source_e::synthetic_black};
    }
    if (role == role_e::monitor) {
      if (!output || output->empty()) {
        return {.source = capture_source_e::invalid};
      }
      return {.source = capture_source_e::exact_output, .output = std::move(output)};
    }
    return {.source = capture_source_e::active_output};
  }

  void register_monitor_runtime_hooks(monitor_runtime_hooks_t hooks) {
    std::lock_guard lock {monitor_runtime_hooks_mutex};
    monitor_runtime_hooks = std::move(hooks);
  }

  monitor_runtime_state_t activate_or_resume_monitor(const std::string_view client_uuid, const std::string_view client_label, const std::string_view requested_mode, const std::uint64_t generation) {
    std::function<monitor_runtime_state_t(std::string_view, std::string_view, std::string_view, std::uint64_t)> activate;
    {
      std::lock_guard lock {monitor_runtime_hooks_mutex};
      activate = monitor_runtime_hooks.activate_or_resume;
    }
    return activate ? activate(client_uuid, client_label, requested_mode, generation) : monitor_runtime_state_t {.retryable = true, .error = "Remote Monitor topology is not ready."};
  }

  monitor_runtime_state_t monitor_runtime_snapshot(const std::string_view client_uuid, const std::uint64_t generation) {
    std::function<monitor_runtime_state_t(std::string_view, std::uint64_t)> snapshot;
    {
      std::lock_guard lock {monitor_runtime_hooks_mutex};
      snapshot = monitor_runtime_hooks.snapshot;
    }
    return snapshot ? snapshot(client_uuid, generation) : monitor_runtime_state_t {.retryable = true, .error = "Remote Monitor topology is not ready."};
  }

  void release_monitor(const std::string_view client_uuid, const std::uint64_t generation, const std::string_view reason) {
    std::function<void(std::string_view, std::uint64_t, std::string_view)> release;
    {
      std::lock_guard lock {monitor_runtime_hooks_mutex};
      release = monitor_runtime_hooks.explicit_release;
    }
    if (release) release(client_uuid, generation, reason);
  }

  void notify_monitor_transport_lost(const std::string_view client_uuid, const std::uint64_t generation) {
    std::function<void(std::string_view, std::uint64_t)> transport_lost;
    {
      std::lock_guard lock {monitor_runtime_hooks_mutex};
      transport_lost = monitor_runtime_hooks.transport_lost;
    }
    if (transport_lost) transport_lost(client_uuid, generation);
  }

  void notify_monitor_unpair(const std::string_view client_uuid) {
    std::function<void(std::string_view)> unpair;
    {
      std::lock_guard lock {monitor_runtime_hooks_mutex};
      unpair = monitor_runtime_hooks.unpair;
    }
    if (unpair) unpair(client_uuid);
  }

  void notify_monitor_shutdown() {
    std::function<void()> shutdown;
    {
      std::lock_guard lock {monitor_runtime_hooks_mutex};
      shutdown = monitor_runtime_hooks.shutdown;
    }
    if (shutdown) shutdown();
  }

  bool pending_registry_t::add(pending_t pending, std::string *warning) {
    expire(std::chrono::steady_clock::now());
    if (!pending.launch_id || pending_.contains(pending.launch_id)) return false;
    if (!pending.encrypted) {
      const auto count = std::count_if(pending_.begin(), pending_.end(), [&pending](const auto &entry) { return !entry.second.encrypted && entry.second.source_address == pending.source_address; });
      if (count != 0) {
        warning_ = "Plaintext RTSP has more than one pending launch for this source address; the new launch was rejected.";
        if (warning) *warning = warning_;
        return false;
      }
    }
    if (pending_.size() >= max_client_vdds * 2) return false;
    pending_.emplace(pending.launch_id, std::move(pending));
    return true;
  }

  std::optional<pending_t> pending_registry_t::match_encrypted(const std::string_view client_uuid, const std::string_view crypto_binding, const std::chrono::steady_clock::time_point now) {
    expire(now);
    std::optional<pending_t> match;
    for (const auto &[id, pending] : pending_) {
      if (pending.encrypted && pending.client_uuid == client_uuid && pending.crypto_binding == crypto_binding) {
        if (match) return std::nullopt;
        match = pending;
      }
    }
    return match;
  }

  std::optional<pending_t> pending_registry_t::match_plaintext(const std::string_view address, const std::chrono::steady_clock::time_point now) {
    expire(now);
    std::optional<pending_t> match;
    for (const auto &[id, pending] : pending_) {
      if (!pending.encrypted && pending.source_address == address) {
        if (match) { warning_ = "Plaintext RTSP source-address routing became ambiguous and was rejected."; return std::nullopt; }
        match = pending;
      }
    }
    return match;
  }
  void pending_registry_t::erase(const std::uint32_t id) { pending_.erase(id); }
  void pending_registry_t::expire(const std::chrono::steady_clock::time_point now) { for (auto it = pending_.begin(); it != pending_.end();) it = it->second.expires_at <= now ? pending_.erase(it) : std::next(it); }
  void pending_registry_t::clear() { pending_.clear(); }
  std::string pending_registry_t::warning() const { return warning_; }

  bool validate_layout(const std::vector<placement_t> &placements, const std::vector<std::string> &known_clients, const std::vector<std::string> &physical_ids, std::string *error) {
    std::unordered_map<std::string, std::string> parents;
    std::unordered_set<std::string> seen;
    bool primary = false;
    for (const auto &placement : placements) {
      if (!contains(known_clients, placement.client_uuid) || !seen.insert(placement.client_uuid).second || placement.client_uuid == placement.anchor_id || placement.gap_px < 0 || placement.gap_px > 100000) { if (error) *error = "Invalid client placement."; return false; }
      if (placement.anchor_kind == "client" ? !contains(known_clients, placement.anchor_id) : placement.anchor_kind != "physical" || !contains(physical_ids, placement.anchor_id)) { if (error) *error = "Unknown layout anchor."; return false; }
      if (placement.edge != "left" && placement.edge != "right" && placement.edge != "above" && placement.edge != "below") { if (error) *error = "Invalid layout edge."; return false; }
      if (placement.alignment != "start" && placement.alignment != "center" && placement.alignment != "end") { if (error) *error = "Invalid layout alignment."; return false; }
      if (placement.primary && primary) { if (error) *error = "Only one client display may be primary."; return false; }
      primary = primary || placement.primary;
      if (placement.anchor_kind == "client") parents.emplace(placement.client_uuid, placement.anchor_id);
    }
    for (const auto &[child, parent] : parents) { std::unordered_set<std::string> path; auto node = child; while (parents.contains(node)) { if (!path.insert(node).second) { if (error) *error = "Client display placement contains a cycle."; return false; } node = parents.at(node); } }
    return true;
  }
}
