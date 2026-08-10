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

  bool validate_layout(const nlohmann::json &layout, const std::vector<std::string> &known_clients, const std::vector<std::string> &known_physical_ids, std::string &error) {
    if (!layout.is_object() || layout.value("version", 0U) != layout_version || !layout.contains("placements") || !layout["placements"].is_object()) {
      error = "Layout must be version 1 with an object of placements.";
      return false;
    }
    unsigned int primary_count = 0;
    std::unordered_map<std::string, std::string> client_anchors;
    const std::set<std::string> edges {"left", "right", "above", "below"};
    const std::set<std::string> alignments {"start", "center", "end"};
    for (const auto &[client, placement] : layout["placements"].items()) {
      if (!contains(known_clients, client) || !placement.is_object()) {
        error = "Every placement must name a paired client.";
        return false;
      }
      const auto anchor_kind = placement.value("anchor_kind", "");
      const auto anchor_id = placement.value("anchor_id", "");
      if ((anchor_kind != "physical" && anchor_kind != "client") || anchor_id.empty() || !edges.contains(placement.value("edge", "")) || !alignments.contains(placement.value("alignment", ""))) {
        error = "Placement has an unsupported anchor, edge, or alignment.";
        return false;
      }
      if (!placement.contains("gap_px") || !placement["gap_px"].is_number_integer() || placement["gap_px"].get<int>() < 0 || placement["gap_px"].get<int>() > max_gap_px) {
        error = "Placement gap_px must be a sane nonnegative integer.";
        return false;
      }
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
      while (auto it = client_anchors.find(current); it != client_anchors.end()) {
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
  void coordinator_t::set_layout(nlohmann::json layout) { std::lock_guard lock(mutex_); layout_ = std::move(layout); }
  void coordinator_t::set_physical_baseline(std::vector<node_t> nodes) { std::lock_guard lock(mutex_); physical_baseline_ = std::move(nodes); }
  std::vector<std::string> coordinator_t::physical_node_ids() const { std::lock_guard lock(mutex_); std::vector<std::string> ids; for (const auto &node : physical_baseline_) ids.push_back(node.id); return ids; }
  void coordinator_t::set_plaintext_rtsp_warning_provider(std::function<std::string(const std::string &)> provider) { std::lock_guard lock(mutex_); plaintext_rtsp_warning_provider_ = std::move(provider); }

  void coordinator_t::note_normal_game_identity(const std::string &client_uuid, const std::string &label, mode_t mode) {
    std::lock_guard lock(mutex_);
    if (!clients_.contains(client_uuid) && clients_.size() >= max_client_identities) return;
    auto &[state] = clients_[client_uuid];
    state.label = label;
    state.requested_mode = mode;
    state.normal_game = true;
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
    auto &[state] = clients_[client_uuid];
    if (generation < state.generation) {
      return {true, state.lifecycle == lifecycle_e::ready, state.lifecycle == lifecycle_e::retryable, state.exact_output, state.warning};
    }
    state.generation = generation;
    state.label = label;
    state.requested_mode = mode;
    state.remote_monitor = true;
    const auto result = activate_locked(client_uuid, state);
    return {result.accepted, result.capture_ready, state.lifecycle == lifecycle_e::retryable, state.exact_output, result.warning};
  }

  monitor_runtime_state_t coordinator_t::snapshot(const std::string &client_uuid, uint64_t generation) const {
    std::lock_guard lock(mutex_);
    const auto it = clients_.find(client_uuid);
    if (it == clients_.end() || generation != it->second.generation) return {};
    const auto &state = it->second;
    return {true, state.lifecycle == lifecycle_e::ready, state.lifecycle == lifecycle_e::retryable, state.exact_output, state.warning};
  }

  bool coordinator_t::is_ready(const std::string &client_uuid, uint64_t generation) const { return snapshot(client_uuid, generation).ready; }

  void coordinator_t::explicit_release(const std::string &client_uuid, uint64_t generation, const std::string &reason) {
    std::lock_guard lock(mutex_);
    const auto it = clients_.find(client_uuid);
    if (it == clients_.end() || generation < it->second.generation) return;
    if (callbacks_.remove_owned_display) callbacks_.remove_owned_display(client_uuid);
    it->second.remote_monitor = false;
    it->second.lease_held = false;
    it->second.lifecycle = lifecycle_e::released;
    it->second.warning = reason;
    if (!it->second.normal_game) clients_.erase(it);
  }

  void coordinator_t::transport_lost(const std::string &client_uuid, uint64_t generation) {
    std::lock_guard lock(mutex_);
    const auto it = clients_.find(client_uuid);
    if (it == clients_.end() || generation < it->second.generation) return;
    it->second.generation = generation;
    it->second.lease_held = false;
    it->second.lifecycle = lifecycle_e::retryable;
    it->second.warning = "Transport was lost; Remote Monitor ownership and desired settings were retained for Resume.";
  }

  activation_result_t coordinator_t::activate_locked(const std::string &client_uuid, client_state_t &state) {
    state.lifecycle = lifecycle_e::leased;
    if (callbacks_.create_or_reclaim && !callbacks_.create_or_reclaim(client_uuid, state.requested_mode)) {
      state.lifecycle = lifecycle_e::retryable;
      state.warning = "The owned virtual display could not be created or reclaimed; Resume will retry the same identity.";
      return {true, false, state.warning};
    }
    state.lease_held = true;
    state.lifecycle = lifecycle_e::applying;
    std::vector<std::string> warnings;
    const auto composed = compose_locked(warnings);
    if (callbacks_.apply_composed_topology && !callbacks_.apply_composed_topology(composed)) {
      state.lifecycle = lifecycle_e::retryable;
      state.warning = "The composed display topology did not apply; existing owners and their displays were retained.";
      return {true, false, state.warning};
    }
    if (!warnings.empty()) state.warning = warnings.front();
    const auto exact_output = callbacks_.exact_target_has_current_mode_and_dxgi ? callbacks_.exact_target_has_current_mode_and_dxgi(client_uuid) : std::nullopt;
    if (!exact_output || exact_output->empty()) {
      state.lifecycle = lifecycle_e::retryable;
      state.warning = "The exact Remote Monitor target is not yet active with a current mode in capture enumeration; no physical-display fallback is allowed.";
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
    if (callbacks_.remove_owned_display) callbacks_.remove_owned_display(client_uuid);
    it->second.remote_monitor = false;
    it->second.lease_held = false;
    it->second.lifecycle = lifecycle_e::released;
    if (!it->second.normal_game) clients_.erase(it);
  }

  void coordinator_t::unpair_client(const std::string &client_uuid) { disconnect_monitor(client_uuid); std::lock_guard lock(mutex_); clients_.erase(client_uuid); }
  void coordinator_t::shutdown() { std::lock_guard lock(mutex_); for (const auto &[uuid, state] : clients_) if (state.remote_monitor && callbacks_.remove_owned_display) callbacks_.remove_owned_display(uuid); clients_.clear(); }

  mode_t coordinator_t::effective_mode(const node_t &node) { return node.current_mode.value_or(node.last_requested_mode.value_or(node.configured_mode)); }

  std::vector<node_t> coordinator_t::compose_locked(std::vector<std::string> &warnings) const {
    auto nodes = physical_baseline_;
    int rightmost = 0;
    for (const auto &node : nodes) rightmost = std::max(rightmost, node.x + effective_mode(node).width);
    for (const auto &[uuid, state] : clients_) {
      if (!state.remote_monitor) continue;
      node_t node {.id = uuid, .label = state.label, .active = true, .configured_mode = state.requested_mode, .last_requested_mode = state.requested_mode};
      const auto placement_it = layout_["placements"].find(uuid);
      if (placement_it == layout_["placements"].end()) { node.x = rightmost; rightmost += effective_mode(node).width; nodes.push_back(std::move(node)); continue; }
      const auto &placement = *placement_it;
      const auto anchor_id = placement.value("anchor_id", "");
      const auto anchor = std::find_if(nodes.begin(), nodes.end(), [&](const node_t &candidate) { return candidate.id == anchor_id; });
      if (anchor == nodes.end()) {
        node.x = rightmost;
        rightmost += effective_mode(node).width;
        warnings.push_back("Saved anchor '" + anchor_id + "' is unavailable; the client display was appended to the right for this activation.");
      } else {
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
        node.primary = placement.value("primary", false);
      }
      nodes.push_back(std::move(node));
    }
    return nodes;
  }

  nlohmann::json coordinator_t::snapshot(const std::vector<nlohmann::json> &paired_clients) const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> warnings;
    const auto nodes = compose_locked(warnings);
    nlohmann::json result {{"status", true}, {"version", layout_version}, {"layout", layout_}, {"capacity", {{"max", max_client_identities}, {"used", clients_.size()}}}, {"warnings", warnings}, {"nodes", nlohmann::json::array()}, {"clients", paired_clients}};
    for (const auto &node : nodes) {
      const auto mode = effective_mode(node);
      result["nodes"].push_back({{"id", node.id}, {"label", node.label}, {"kind", node.physical ? "physical" : "client"}, {"active", node.active}, {"primary", node.primary}, {"desired_position", {{"x", node.x}, {"y", node.y}}}, {"current_position", {{"x", node.x}, {"y", node.y}}}, {"mode", {{"width", mode.width}, {"height", mode.height}, {"refresh_hz", mode.refresh_hz}}}});
    }
    for (const auto &[uuid, state] : clients_) result["runtime"][uuid] = {{"lifecycle", lifecycle_name(state.lifecycle)}, {"ready", state.lifecycle == lifecycle_e::ready}, {"retryable", state.lifecycle == lifecycle_e::retryable}, {"lease_held", state.lease_held}, {"warning", state.warning}, {"plaintext_rtsp_warning", plaintext_rtsp_warning_provider_ ? plaintext_rtsp_warning_provider_(uuid) : ""}};
    return result;
  }
}  // namespace remote_display_topology
