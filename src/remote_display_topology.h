#pragma once

#include <functional>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace remote_display_topology {

  inline constexpr unsigned int layout_version = 1;
  inline constexpr std::size_t max_client_identities = 4;

  enum class lifecycle_e { desired, leased, applying, ready, retryable, released };

  struct mode_t {
    int width = 1920;
    int height = 1080;
    int refresh_hz = 60;
  };

  struct node_t {
    std::string id;
    std::string label;
    bool physical = false;
    bool active = false;
    bool primary = false;
    int x = 0;
    int y = 0;
    mode_t configured_mode;
    std::optional<mode_t> last_requested_mode;
    std::optional<mode_t> current_mode;
  };

  struct runtime_callbacks_t {
    std::function<bool(const std::string &client_uuid, const std::string &client_label, const mode_t &mode)> create_or_reclaim;
    std::function<bool(const std::vector<node_t> &composed)> apply_composed_topology;
    // The output is accepted only after this exact client identity has both the
    // requested mode and a capture-enumerated DXGI name.  Passing the mode here
    // prevents a callback from treating a GUID/device-id lookup as readiness.
    std::function<std::optional<std::string>(const std::string &client_uuid, const mode_t &mode)> exact_target_has_current_mode_and_dxgi;
    std::function<void(const std::string &client_uuid)> remove_owned_display;
  };

  struct activation_result_t {
    bool accepted = false;
    bool capture_ready = false;
    std::string warning;
  };

  struct monitor_runtime_state_t {
    bool accepted = false;
    bool ready = false;
    bool retryable = false;
    std::string output;
    std::string error;
  };

  struct normal_game_reservation_t {
    bool accepted = false;
    bool newly_reserved = false;
    std::uint64_t token = 0;
  };

  // Validates the persisted wire contract without mutating coordinator state.
  bool validate_layout(const nlohmann::json &layout, const std::vector<std::string> &known_clients, const std::vector<std::string> &known_physical_ids, std::string &error);

  class coordinator_t {
  public:
    void set_runtime_callbacks(runtime_callbacks_t callbacks);
    void set_layout(nlohmann::json layout);
    void set_physical_baseline(std::vector<node_t> nodes);
    std::vector<std::string> physical_node_ids() const;
    // Managed ownership is independent of transport lifetime. A retryable or
    // transport-less Remote Monitor remains protected until an explicit owner
    // release, and a normal game may share the same stable client identity.
    std::size_t managed_client_identity_count() const;
    std::vector<std::string> managed_client_identity_ids() const;
    std::vector<std::string> protected_remote_monitor_client_ids() const;
    bool generic_virtual_display_cleanup_allowed() const;
    void set_plaintext_rtsp_warning_provider(std::function<std::string(const std::string &)> provider);
    monitor_runtime_state_t activate_or_resume(const std::string &client_uuid, const std::string &label, mode_t mode, uint64_t generation);
    monitor_runtime_state_t snapshot(const std::string &client_uuid, uint64_t generation) const;
    bool is_ready(const std::string &client_uuid, uint64_t generation) const;
    void explicit_release(const std::string &client_uuid, uint64_t generation, const std::string &reason);
    void transport_lost(const std::string &client_uuid, uint64_t generation);
    activation_result_t activate_remote_monitor(const std::string &client_uuid, const std::string &label, mode_t mode);
    activation_result_t resume_remote_monitor(const std::string &client_uuid);
    normal_game_reservation_t reserve_normal_game_identity(const std::string &client_uuid, const std::string &label, mode_t mode);
    void rollback_normal_game_identity(const std::string &client_uuid, std::uint64_t token);
    void release_normal_game_identity(const std::string &client_uuid, std::uint64_t token);
    void note_lease_lost(const std::string &client_uuid);
    void disconnect_monitor(const std::string &client_uuid);
    void unpair_client(const std::string &client_uuid);
    void shutdown();
    nlohmann::json snapshot(const std::vector<nlohmann::json> &paired_clients) const;

  private:
    struct client_state_t {
      std::string label;
      mode_t requested_mode;
      bool normal_game = false;
      std::uint64_t normal_game_token = 0;
      bool remote_monitor = false;
      bool lease_held = false;
      uint64_t generation = 0;
      std::string exact_output;
      lifecycle_e lifecycle = lifecycle_e::desired;
      std::string warning;
    };

    activation_result_t activate_locked(const std::string &client_uuid, client_state_t &state);
    void release_locked(const std::string &client_uuid, client_state_t &state, const std::string &reason);
    std::vector<node_t> compose_locked(std::vector<std::string> &warnings) const;
    static mode_t effective_mode(const node_t &node);
    mutable std::mutex mutex_;
    runtime_callbacks_t callbacks_;
    std::function<std::string(const std::string &)> plaintext_rtsp_warning_provider_;
    nlohmann::json layout_ { {"version", layout_version}, {"placements", nlohmann::json::object()} };
    std::vector<node_t> physical_baseline_;
    std::unordered_map<std::string, client_state_t> clients_;
    std::uint64_t next_normal_game_token_ = 0;
  };

  coordinator_t &instance();
}  // namespace remote_display_topology
