#include "remote_session.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <unordered_set>

namespace remote_session {
  namespace {
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

  bool reserved_name(const std::string_view name) { return equal_folded(name, "Remote Input") || equal_folded(name, "Remote Monitor"); }

  control_e identify(const std::int32_t id, const std::string_view uuid) {
    if (id == resume_id || id == 2147483601 || uuid == "9a1c5a25-58fe-40e0-b9aa-7d3f00000001") return control_e::resume;
    if (id == disconnect_monitor_id || id == 2147483602 || uuid == "9a1c5a25-58fe-40e0-b9aa-7d3f00000002") return control_e::disconnect_monitor;
    if (id == disconnect_input_id || id == 2147483603 || uuid == "9a1c5a25-58fe-40e0-b9aa-7d3f00000003") return control_e::disconnect_input;
    if (id == disconnect_game_id || id == 2147483604 || uuid == "9a1c5a25-58fe-40e0-b9aa-7d3f00000004") return control_e::disconnect_game;
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
      case control_e::disconnect_game: return {disconnect_game_id, synthetic_uuid(control), "Disconnect", true};
      case control_e::monitor: return {monitor_id, synthetic_uuid(control), "Remote Monitor", true};
      case control_e::input: return {input_id, synthetic_uuid(control), "Remote Input", true};
      default: return {};
    }
  }

  projection_t project(const caller_t &caller, const game_t &game, const owner_t &owner, const std::vector<app_t> &configured) {
    projection_t result;
    if (owns_game(caller, game)) {
      result.free = false;
      result.current_game = game.app.id;
      result.catalogue = configured;
      result.catalogue.push_back(synthetic(control_e::input));
      result.catalogue.push_back(synthetic(control_e::monitor));
      return result;
    }
    if (owner.role == role_e::monitor) {
      result.catalogue = {synthetic(control_e::resume), synthetic(control_e::disconnect_monitor)};
      return result;
    }
    if (owner.role == role_e::input) {
      result.catalogue = {synthetic(control_e::disconnect_input)};
      return result;
    }
    if (game.running) {
      result.catalogue = {synthetic(control_e::resume), synthetic(control_e::disconnect_game), game.app, synthetic(control_e::input), synthetic(control_e::monitor)};
      return result;
    }
    result.catalogue = configured;
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
        break;
      case control_e::input:
      case control_e::monitor:
        result.permission = permission_e::launch;
        result.allowed = caller.may_launch;
        break;
      case control_e::disconnect_game:
        result.permission = permission_e::terminate;
        result.allowed = caller.may_terminate && game.running;
        result.terminate_game = result.allowed;
        break;
      case control_e::disconnect_monitor:
        result.permission = permission_e::terminate;
        result.allowed = caller.may_terminate && owner.role == role_e::monitor;
        break;
      case control_e::disconnect_input:
        result.permission = permission_e::terminate;
        result.allowed = caller.may_terminate && owner.role == role_e::input;
        break;
      default: break;
    }
    return result;
  }

  bool input_uses_display_or_audio(const role_e role) { return role != role_e::input; }

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

  std::optional<pending_t> pending_registry_t::match_encrypted(const std::string_view client_uuid, const std::chrono::steady_clock::time_point now) {
    expire(now);
    std::optional<pending_t> match;
    for (const auto &[id, pending] : pending_) {
      if (pending.encrypted && pending.client_uuid == client_uuid) {
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
