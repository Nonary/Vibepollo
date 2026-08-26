#include "remote_display_topology.h"

#include <algorithm>
#include <set>
#include <unordered_set>

namespace remote_display_topology {
  namespace {
    constexpr int max_gap_px = 100000;

    bool contains(const std::vector<std::string> &values, const std::string &value) {
      return std::find(values.begin(), values.end(), value) != values.end();
    }

    const char *lifecycle_name(lifecycle_e lifecycle) {
      switch (lifecycle) {
        case lifecycle_e::desired: return "desired";
        case lifecycle_e::leased: return "leased";
        case lifecycle_e::applying: return "applying";
        case lifecycle_e::ready: return "ready";
        case lifecycle_e::retryable: return "retryable";
        case lifecycle_e::released: return "released";
      }
      return "retryable";
    }
  }  // namespace

  namespace {
    nlohmann::json empty_layout() { return {{"version", layout_version}, {"placements", nlohmann::json::object()}}; }
  }

  bool layout_schema_valid(const nlohmann::json &layout, std::string &error) {
    if (!layout.is_object() || !layout.contains("version") || !layout["version"].is_number_integer() || layout["version"].get<int>() != static_cast<int>(layout_version) || !layout.contains("placements") || !layout["placements"].is_object()) {
      error = "Layout must be version 1 with an object of placements.";
      return false;
    }
    const std::set<std::string> edges {"left", "right", "above", "below"};
    const std::set<std::string> alignments {"start", "center", "end"};
    for (const auto &[client, placement] : layout["placements"].items()) {
      if (client.empty() || !placement.is_object() || !placement.contains("anchor_kind") || !placement["anchor_kind"].is_string() || !placement.contains("anchor_id") || !placement["anchor_id"].is_string() || !placement.contains("edge") || !placement["edge"].is_string() || !placement.contains("alignment") || !placement["alignment"].is_string() || !placement.contains("gap_px") || !placement["gap_px"].is_number_integer()) {
        error = "Every layout placement must use the expected field types.";
        return false;
      }
      if (placement.contains("primary") && !placement["primary"].is_boolean()) {
        error = "Placement primary must be a boolean.";
        return false;
      }
      const auto gap = placement["gap_px"].get<int>();
      if ((placement["anchor_kind"].get<std::string>() != "physical" && placement["anchor_kind"].get<std::string>() != "client") || placement["anchor_id"].get<std::string>().empty() || !edges.contains(placement["edge"].get<std::string>()) || !alignments.contains(placement["alignment"].get<std::string>()) || gap < 0 || gap > max_gap_px) {
        error = "Placement has an unsupported anchor, edge, alignment, or gap.";
        return false;
      }
    }
    return true;
  }

  nlohmann::json normalize_layout(const nlohmann::json &layout) {
    std::string error;
    return layout_schema_valid(layout, error) ? layout : empty_layout();
  }

  bool validate_layout(const nlohmann::json &layout, const std::vector<std::string> &known_clients, const std::vector<std::string> &known_physical_ids, std::string &error) {
    if (!layout_schema_valid(layout, error)) return false;
    unsigned int primary_count = 0;
    std::unordered_map<std::string, std::string> client_anchors;
    const std::set<std::string> edges {"left", "right", "above", "below"};
    const std::set<std::string> alignments {"start", "center", "end"};
    for (const auto &[client, placement] : layout["placements"].items()) {
      if (!contains(known_clients, client) || !placement.is_object()) {
        error = "Every placement must name a paired client.";
        return false;
      }
      const auto anchor_kind = placement["anchor_kind"].get<std::string>();
      const auto anchor_id = placement["anchor_id"].get<std::string>();
      if (placement.value("primary", false) && ++primary_count > 1) {
        error = "Only one client display may be primary.";
        return false;
      }
      if (anchor_kind == "client") {
        if (!contains(known_clients, anchor_id) || anchor_id == client) {
          error = "A client placement cannot anchor to itself or an unknown client.";
          return false;
        }
        client_anchors.emplace(client, anchor_id);
      } else if (!contains(known_physical_ids, anchor_id)) {
        error = "A physical placement must anchor to a currently known monitor device path.";
        return false;
      }
    }
    for (const auto &[start, ignored] : client_anchors) {
      std::unordered_set<std::string> visited;
      auto current = start;
      for (auto it = client_anchors.find(current); it != client_anchors.end(); it = client_anchors.find(current)) {
        if (!visited.insert(current).second) {
          error = "Client placements must not contain an anchor cycle.";
          return false;
        }
        current = it->second;
      }
    }
    return true;
  }

  coordinator_t &instance() { static coordinator_t coordinator; return coordinator; }

  void coordinator_t::set_runtime_callbacks(runtime_callbacks_t callbacks) { std::lock_guard lock(mutex_); callbacks_ = std::move(callbacks); }
  void coordinator_t::set_layout(nlohmann::json layout) { std::lock_guard lock(mutex_); layout_ = normalize_layout(layout); }
  void coordinator_t::set_physical_baseline(std::vector<node_t> nodes) { std::lock_guard lock(mutex_); physical_baseline_ = std::move(nodes); }
  std::vector<std::string> coordinator_t::physical_node_ids() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> ids;
    for (const auto &node : physical_baseline_) {
      if (node.physical) ids.push_back(node.id);
    }
    return ids;
  }
  std::size_t coordinator_t::managed_client_identity_count() const {
    std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(std::count_if(clients_.begin(), clients_.end(), [](const auto &entry) {
      return entry.second.normal_game || entry.second.remote_monitor;
    }));
  }
  std::vector<std::string> coordinator_t::managed_client_identity_ids() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> ids;
    for (const auto &[uuid, state] : clients_) {
      if (state.normal_game || state.remote_monitor) ids.push_back(uuid);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  }
  std::vector<std::string> coordinator_t::protected_remote_monitor_client_ids() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> ids;
    for (const auto &[uuid, state] : clients_) {
      if (state.remote_monitor) ids.push_back(uuid);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  }
  bool coordinator_t::generic_virtual_display_cleanup_allowed() const {
    std::lock_guard lock(mutex_);
    return std::none_of(clients_.begin(), clients_.end(), [](const auto &entry) {
      return entry.second.normal_game || entry.second.remote_monitor;
    });
  }
  void coordinator_t::set_plaintext_rtsp_warning_provider(std::function<std::string(const std::string &)> provider) { std::lock_guard lock(mutex_); plaintext_rtsp_warning_provider_ = std::move(provider); }

  normal_game_reservation_t coordinator_t::reserve_normal_game_identity(const std::string &client_uuid, const std::string &label, mode_t mode) {
    std::lock_guard lock(mutex_);
    if (client_uuid.empty() || (!clients_.contains(client_uuid) && clients_.size() >= max_client_identities)) return {};
    auto [state_it, inserted] = clients_.try_emplace(client_uuid);
    auto &state = state_it->second;
    if (inserted) state.placement_order = ++next_placement_order_;
    if (state.normal_game) return {true, false, state.normal_game_token};
    state.label = label;
    state.normal_requested_mode = mode;
    if (!state.remote_monitor) state.effective_mode = mode;
    state.normal_game = true;
    state.normal_game_token = ++next_normal_game_token_;
    return {true, true, state.normal_game_token};
  }

  bool coordinator_t::reapply_composed_topology() {
    std::lock_guard lock(mutex_);
    if (!callbacks_.apply_composed_topology) {
      return false;
    }
    if (callbacks_.resolve_mode) {
      for (auto &[uuid, state] : clients_) {
        if (state.normal_game || state.remote_monitor) {
          resolve_effective_mode_locked(uuid, state);
        }
      }
    } else {
      for (auto &[_, state] : clients_) {
        if (state.normal_game || state.remote_monitor) state.effective_mode = desired_mode(state);
      }
    }
    std::vector<std::string> ignored;
    return callbacks_.apply_composed_topology(compose_locked(ignored));
  }

  void coordinator_t::rollback_normal_game_identity(const std::string &client_uuid, const std::uint64_t token) {
    std::lock_guard lock(mutex_);
    const auto it = clients_.find(client_uuid);
    if (it == clients_.end() || !it->second.normal_game || it->second.normal_game_token != token) return;
    it->second.normal_game = false;
    it->second.normal_requested_mode.reset();
    it->second.normal_game_token = 0;
    if (!it->second.remote_monitor) clients_.erase(it);
  }

  void coordinator_t::release_normal_game_identity(const std::string &client_uuid, const std::uint64_t token) {
    std::lock_guard lock(mutex_);
    const auto it = clients_.find(client_uuid);
    if (it == clients_.end() || !it->second.normal_game || token == 0 || it->second.normal_game_token != token) return;
    it->second.normal_game = false;
    it->second.normal_requested_mode.reset();
    it->second.normal_game_token = 0;
    if (it->second.remote_monitor) return;

    if (callbacks_.remove_owned_display) callbacks_.remove_owned_display(client_uuid);
    clients_.erase(it);
    if (callbacks_.apply_composed_topology) {
      std::vector<std::string> ignored;
      (void) callbacks_.apply_composed_topology(compose_locked(ignored));
    }
  }

  activation_result_t coordinator_t::activate_remote_monitor(const std::string &client_uuid, const std::string &label, mode_t mode) {
    const auto result = activate_or_resume(client_uuid, label, mode, 0);
    return {result.accepted, result.ready, result.error};
  }

  activation_result_t coordinator_t::resume_remote_monitor(const std::string &client_uuid) {
    std::lock_guard lock(mutex_);
    const auto it = clients_.find(client_uuid);
    if (it == clients_.end() || !it->second.remote_monitor) return {false, false, "Remote Monitor is not owned by this paired client."};
    return activate_locked(client_uuid, it->second);
  }

  monitor_runtime_state_t coordinator_t::activate_or_resume(const std::string &client_uuid, const std::string &label, mode_t mode, uint64_t generation) {
    std::lock_guard lock(mutex_);
    if (!clients_.contains(client_uuid) && clients_.size() >= max_client_identities) {
      return {false, false, true, {}, "Remote display capacity is four paired-client identities."};
    }
    auto [state_it, inserted] = clients_.try_emplace(client_uuid);
    auto &state = state_it->second;
    if (inserted) state.placement_order = ++next_placement_order_;
    if (generation < state.generation) {
      return {true, state.lifecycle == lifecycle_e::ready, state.lifecycle == lifecycle_e::retryable, state.exact_output, state.warning, state.effective_mode.hdr};
    }
    state.generation = generation;
    state.label = label;
    state.monitor_requested_mode = mode;
    state.effective_mode = mode;
    state.remote_monitor = true;
    const auto result = activate_locked(client_uuid, state);
    return {result.accepted, result.capture_ready, state.lifecycle == lifecycle_e::retryable, state.exact_output, result.warning, state.effective_mode.hdr};
  }

  monitor_runtime_state_t coordinator_t::snapshot(const std::string &client_uuid, uint64_t generation) const {
    std::lock_guard lock(mutex_);
    const auto it = clients_.find(client_uuid);
    if (it == clients_.end() || generation != it->second.generation) return {};
    const auto &state = it->second;
    return {true, state.lifecycle == lifecycle_e::ready, state.lifecycle == lifecycle_e::retryable, state.exact_output, state.warning, state.effective_mode.hdr};
  }

  bool coordinator_t::is_ready(const std::string &client_uuid, uint64_t generation) const { return snapshot(client_uuid, generation).ready; }

  void coordinator_t::explicit_release(const std::string &client_uuid, uint64_t generation, const std::string &reason) {
    std::lock_guard lock(mutex_);
    const auto it = clients_.find(client_uuid);
    if (it == clients_.end() || generation != it->second.generation) return;
    release_locked(client_uuid, it->second, reason);
    if (!it->second.normal_game) clients_.erase(it);
  }

  void coordinator_t::transport_lost(const std::string &client_uuid, uint64_t generation) {
    std::lock_guard lock(mutex_);
    const auto it = clients_.find(client_uuid);
    if (it == clients_.end() || generation != it->second.generation) return;
    it->second.lifecycle = lifecycle_e::retryable;
    it->second.warning = "Transport was lost; Remote Monitor ownership and desired settings were retained for Resume.";
  }

  activation_result_t coordinator_t::activate_locked(const std::string &client_uuid, client_state_t &state) {
    if (!callbacks_.create_or_reclaim || !callbacks_.apply_composed_topology || !callbacks_.exact_target_has_current_mode_and_dxgi) {
      state.lifecycle = lifecycle_e::retryable;
      state.warning = "Remote Monitor runtime wiring is unavailable; ownership was retained for Resume.";
      return {true, false, state.warning};
    }
    if (!state.lease_held) {
      state.lifecycle = lifecycle_e::leased;
      if (!callbacks_.create_or_reclaim(client_uuid, state.label, desired_mode(state))) {
        state.lifecycle = lifecycle_e::retryable;
        state.warning = "The owned virtual display could not be created or reclaimed; Resume will retry the same identity.";
        return {true, false, state.warning};
      }
      state.lease_held = true;
    }
    resolve_effective_mode_locked(client_uuid, state);
    state.lifecycle = lifecycle_e::applying;
    std::vector<std::string> warnings;
    const auto composed = compose_locked(warnings);
    if (!callbacks_.apply_composed_topology(composed)) {
      state.lifecycle = lifecycle_e::retryable;
      state.warning = "The composed display topology did not apply; existing owners and their displays were retained.";
      return {true, false, state.warning};
    }
    if (!warnings.empty()) state.warning = warnings.front();
    const auto exact_output = callbacks_.exact_target_has_current_mode_and_dxgi(client_uuid, state.effective_mode);
    if (!exact_output || exact_output->empty()) {
      state.lifecycle = lifecycle_e::retryable;
      state.warning = "The requested client display is not yet capture-ready. Existing streamed physical or virtual displays remain topology anchors, but capture will wait rather than mirror another screen.";
      return {true, false, state.warning};
    }
    state.exact_output = *exact_output;
    state.lifecycle = lifecycle_e::ready;
    state.warning.clear();
    return {true, true, {}};
  }

  void coordinator_t::note_lease_lost(const std::string &client_uuid) {
    std::lock_guard lock(mutex_);
    if (auto it = clients_.find(client_uuid); it != clients_.end()) {
      it->second.lease_held = false;
      it->second.lifecycle = lifecycle_e::retryable;
      it->second.warning = "Remote Monitor lease was lost; desired settings and identity are retained for Resume.";
    }
  }

  void coordinator_t::disconnect_monitor(const std::string &client_uuid) {
    std::lock_guard lock(mutex_);
    const auto it = clients_.find(client_uuid);
    if (it == clients_.end()) return;
    release_locked(client_uuid, it->second, "Remote Monitor was disconnected.");
    if (!it->second.normal_game) clients_.erase(it);
  }

  void coordinator_t::unpair_client(const std::string &client_uuid) {
    // Unpair releases Remote Monitor ownership, but it cannot discard a normal
    // game's still-live ownership record. The process teardown releases that
    // role with its reservation token.
    disconnect_monitor(client_uuid);
  }
  void coordinator_t::shutdown() {
    std::lock_guard lock(mutex_);
    std::vector<std::string> managed_ids;
    for (const auto &[uuid, state] : clients_) {
      if (state.normal_game || state.remote_monitor) managed_ids.push_back(uuid);
    }
    std::sort(managed_ids.begin(), managed_ids.end());
    for (const auto &uuid : managed_ids) {
      if (callbacks_.remove_owned_display) callbacks_.remove_owned_display(uuid);
    }
    clients_.clear();
    if (callbacks_.apply_composed_topology) {
      std::vector<std::string> ignored;
      (void) callbacks_.apply_composed_topology(compose_locked(ignored));
    }
  }

  void coordinator_t::release_locked(const std::string &client_uuid, client_state_t &state, const std::string &reason) {
    if (!state.remote_monitor) return;
    // A normal game and Remote Monitor for one paired client share the same
    // deterministic VDD. Ending either role cannot remove the other's display.
    if (!state.normal_game && callbacks_.remove_owned_display) callbacks_.remove_owned_display(client_uuid);
    state.remote_monitor = false;
    state.monitor_requested_mode.reset();
    if (state.normal_game) {
      resolve_effective_mode_locked(client_uuid, state);
    }
    // The normal-game role still owns the shared connector. Keep the lease
    // truthfully held so a later monitor resume cannot appear to reacquire or
    // disconnect it out from under the game.
    state.lease_held = state.normal_game;
    state.lifecycle = lifecycle_e::released;
    state.warning = reason;

    // Recompose only the remaining explicit owners.  This intentionally does
    // not restore a saved/global topology or remove any peer identity.
    if (callbacks_.apply_composed_topology) {
      std::vector<std::string> ignored;
      (void) callbacks_.apply_composed_topology(compose_locked(ignored));
    }
  }

  mode_t coordinator_t::effective_mode(const node_t &node) { return node.current_mode.value_or(node.last_requested_mode.value_or(node.configured_mode)); }

  mode_t coordinator_t::desired_mode(const client_state_t &state) {
    if (state.remote_monitor && state.monitor_requested_mode) return *state.monitor_requested_mode;
    if (state.normal_game && state.normal_requested_mode) return *state.normal_requested_mode;
    return {};
  }

  void coordinator_t::resolve_effective_mode_locked(const std::string &client_uuid, client_state_t &state) {
    state.effective_mode = desired_mode(state);
    if (callbacks_.resolve_mode) callbacks_.resolve_mode(client_uuid, state.effective_mode);
  }

  std::vector<node_t> coordinator_t::compose_locked(std::vector<std::string> &warnings) const {
    auto nodes = physical_baseline_;
    int rightmost = 0;
    for (const auto &node : nodes) rightmost = std::max(rightmost, node.x + effective_mode(node).width);

    std::vector<std::string> active_ids;
    for (const auto &[uuid, state] : clients_) {
      if (state.remote_monitor || state.normal_game) active_ids.push_back(uuid);
    }
    std::sort(active_ids.begin(), active_ids.end(), [&](const std::string &lhs, const std::string &rhs) {
      const auto lhs_order = clients_.at(lhs).placement_order;
      const auto rhs_order = clients_.at(rhs).placement_order;
      return lhs_order == rhs_order ? lhs < rhs : lhs_order < rhs_order;
    });

    std::unordered_set<std::string> emitted;
    std::unordered_set<std::string> visiting;
    std::function<void(const std::string &)> emit = [&](const std::string &uuid) {
      if (emitted.contains(uuid)) return;
      const auto state_it = clients_.find(uuid);
      if (state_it == clients_.end() || (!state_it->second.remote_monitor && !state_it->second.normal_game)) return;

      node_t node {
        .id = uuid,
        .label = state_it->second.label,
        .active = true,
        .configured_mode = state_it->second.effective_mode,
        .last_requested_mode = state_it->second.effective_mode,
      };
      const auto append_right = [&](const std::string &warning) {
        node.x = rightmost;
        rightmost += effective_mode(node).width;
        if (!warning.empty()) warnings.push_back(warning);
        nodes.push_back(std::move(node));
        emitted.insert(uuid);
      };

      if (!visiting.insert(uuid).second) {
        append_right("Saved client anchors contain a cycle; '" + uuid + "' was appended to the right for this activation.");
        return;
      }

      const auto placement_it = layout_["placements"].find(uuid);
      if (placement_it == layout_["placements"].end()) {
        append_right({});
        visiting.erase(uuid);
        return;
      }

      const auto &placement = *placement_it;
      node.primary = placement.value("primary", false);
      const auto anchor_id = placement.value("anchor_id", "");
      if (placement.value("anchor_kind", "") == "client") {
        emit(anchor_id);
        if (emitted.contains(uuid)) {
          visiting.erase(uuid);
          return;
        }
      }

      const auto anchor = std::find_if(nodes.begin(), nodes.end(), [&](const node_t &candidate) { return candidate.id == anchor_id; });
      if (anchor == nodes.end()) {
        append_right("Saved anchor '" + anchor_id + "' is unavailable; the client display was appended to the right for this activation.");
        visiting.erase(uuid);
        return;
      }

      const auto anchor_mode = effective_mode(*anchor);
      const auto mode = effective_mode(node);
      const auto gap = placement.value("gap_px", 0);
      const auto edge = placement.value("edge", "right");
      const auto alignment = placement.value("alignment", "center");
      if (edge == "left") node.x = anchor->x - mode.width - gap;
      if (edge == "right") node.x = anchor->x + anchor_mode.width + gap;
      if (edge == "above") node.y = anchor->y - mode.height - gap;
      if (edge == "below") node.y = anchor->y + anchor_mode.height + gap;
      if (edge == "left" || edge == "right") node.y = alignment == "start" ? anchor->y : alignment == "end" ? anchor->y + anchor_mode.height - mode.height : anchor->y + (anchor_mode.height - mode.height) / 2;
      if (edge == "above" || edge == "below") node.x = alignment == "start" ? anchor->x : alignment == "end" ? anchor->x + anchor_mode.width - mode.width : anchor->x + (anchor_mode.width - mode.width) / 2;
      rightmost = std::max(rightmost, node.x + mode.width);
      nodes.push_back(std::move(node));
      emitted.insert(uuid);
      visiting.erase(uuid);
    };

    for (const auto &uuid : active_ids) emit(uuid);
    return nodes;
  }

  nlohmann::json coordinator_t::snapshot(const std::vector<nlohmann::json> &paired_clients) const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> warnings;
    const auto nodes = compose_locked(warnings);
    nlohmann::json result {{"status", true}, {"version", layout_version}, {"layout", layout_}, {"capacity", {{"max", max_client_identities}, {"used", clients_.size()}}}, {"warnings", warnings}, {"nodes", nlohmann::json::array()}, {"clients", paired_clients}};
    for (const auto &node : nodes) {
      const auto mode = effective_mode(node);
      result["nodes"].push_back({{"id", node.id}, {"label", node.label}, {"kind", node.physical ? "physical" : "client"}, {"active", node.active}, {"primary", node.primary}, {"desired_position", {{"x", node.x}, {"y", node.y}}}, {"current_position", {{"x", node.x}, {"y", node.y}}}, {"mode", {{"width", mode.width}, {"height", mode.height}, {"refresh_hz", mode.refresh_hz}, {"hdr", mode.hdr}}}});
    }
    for (const auto &[uuid, state] : clients_) result["runtime"][uuid] = {{"lifecycle", lifecycle_name(state.lifecycle)}, {"ready", state.lifecycle == lifecycle_e::ready}, {"retryable", state.lifecycle == lifecycle_e::retryable}, {"lease_held", state.lease_held}, {"warning", state.warning}, {"plaintext_rtsp_warning", plaintext_rtsp_warning_provider_ ? plaintext_rtsp_warning_provider_(uuid) : ""}};
    return result;
  }
}  // namespace remote_display_topology
