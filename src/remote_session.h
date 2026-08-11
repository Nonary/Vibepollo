#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace remote_session {
  inline constexpr std::int32_t resume_id = 2147483501;
  inline constexpr std::int32_t disconnect_monitor_id = 2147483502;
  inline constexpr std::int32_t disconnect_input_id = 2147483503;
  inline constexpr std::int32_t disconnect_game_id = 2147483504;
  inline constexpr std::int32_t monitor_id = 2147483505;
  inline constexpr std::int32_t input_id = 2147483506;
  inline constexpr std::size_t max_client_vdds = 4;

  enum class role_e : std::uint8_t { none, input, monitor, game };
  enum class control_e : std::uint8_t { none, resume, disconnect_monitor, disconnect_input, disconnect_game, monitor, input, running_game };
  enum class permission_e : std::uint8_t { view, launch, terminate };

  struct app_t {
    std::int32_t id {};
    std::string uuid;
    std::string title;
    bool synthetic {};
  };

  struct game_t {
    bool running {};
    std::string owner_uuid;
    app_t app;
  };

  struct owner_t {
    role_e role {role_e::none};
    bool retained {};
    bool ready {};
    bool retryable {};
    std::optional<std::string> output;
  };

  struct caller_t {
    std::string uuid;
    bool paired {};
    bool may_view {};
    bool may_launch {};
    bool may_terminate {};
  };

  struct projection_t {
    bool free {true};
    std::int32_t current_game {};
    std::vector<app_t> catalogue;
  };

  struct dispatch_t {
    control_e control {control_e::none};
    permission_e permission {permission_e::view};
    bool allowed {};
    bool resume {};
    bool terminate_game {};
  };

  struct pending_t {
    std::uint32_t launch_id {};
    std::string client_uuid;
    std::string crypto_binding;
    std::string source_address;
    bool encrypted {};
    role_e role {role_e::game};
    std::uint64_t generation {};
    std::chrono::steady_clock::time_point expires_at {};
  };

  struct monitor_runtime_state_t {
    bool accepted {};
    bool ready {};
    bool retryable {};
    std::string output;
    std::string error;
  };

  struct monitor_runtime_hooks_t {
    std::function<monitor_runtime_state_t(std::string_view client_uuid, std::string_view client_label, std::string_view requested_mode, std::uint64_t generation)> activate_or_resume;
    std::function<monitor_runtime_state_t(std::string_view client_uuid, std::uint64_t generation)> snapshot;
    std::function<void(std::string_view client_uuid, std::uint64_t generation, std::string_view reason)> explicit_release;
    std::function<void(std::string_view client_uuid, std::uint64_t generation)> transport_lost;
    std::function<void(std::string_view client_uuid)> unpair;
    std::function<void()> shutdown;
  };

  struct placement_t {
    std::string client_uuid;
    std::string anchor_kind;
    std::string anchor_id;
    std::string edge;
    std::string alignment;
    int gap_px {};
    bool primary {};
  };

  [[nodiscard]] bool reserved_name(std::string_view name);
  [[nodiscard]] control_e identify(std::int32_t id, std::string_view uuid = {});
  [[nodiscard]] std::string synthetic_uuid(control_e control);
  [[nodiscard]] app_t synthetic(control_e control);
  [[nodiscard]] projection_t project(const caller_t &caller, const game_t &game, const owner_t &owner, const std::vector<app_t> &configured);
  [[nodiscard]] dispatch_t dispatch(const caller_t &caller, const game_t &game, const owner_t &owner, control_e control);
  [[nodiscard]] bool input_uses_display_or_audio(role_e role);

  class normal_app_transition_gate_t {
  public:
    void lock() { mutex_.lock(); }
    bool try_lock() { return mutex_.try_lock(); }
    void unlock() { mutex_.unlock(); }

  private:
    std::mutex mutex_;
  };

  void register_monitor_runtime_hooks(monitor_runtime_hooks_t hooks);
  [[nodiscard]] monitor_runtime_state_t activate_or_resume_monitor(std::string_view client_uuid, std::string_view client_label, std::string_view requested_mode, std::uint64_t generation);
  [[nodiscard]] monitor_runtime_state_t monitor_runtime_snapshot(std::string_view client_uuid, std::uint64_t generation);
  void release_monitor(std::string_view client_uuid, std::uint64_t generation, std::string_view reason);
  void notify_monitor_transport_lost(std::string_view client_uuid, std::uint64_t generation);
  void notify_monitor_unpair(std::string_view client_uuid);
  void notify_monitor_shutdown();

  class pending_registry_t {
  public:
    bool add(pending_t pending, std::string *warning = nullptr);
    std::optional<pending_t> match_encrypted(std::string_view client_uuid, std::string_view crypto_binding, std::chrono::steady_clock::time_point now);
    std::optional<pending_t> match_plaintext(std::string_view address, std::chrono::steady_clock::time_point now);
    void erase(std::uint32_t id);
    void expire(std::chrono::steady_clock::time_point now);
    void clear();
    [[nodiscard]] std::string warning() const;
  private:
    std::unordered_map<std::uint32_t, pending_t> pending_;
    std::string warning_;
  };

  [[nodiscard]] bool validate_layout(const std::vector<placement_t> &placements, const std::vector<std::string> &known_clients, const std::vector<std::string> &physical_ids, std::string *error);
}
