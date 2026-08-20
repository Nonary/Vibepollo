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
  // Keep this numeric identity stable so clients with a cached app tile keep
  // resolving the control after its user-facing name changes to Terminate.
  inline constexpr std::int32_t terminate_id = 2147483504;
  inline constexpr std::int32_t monitor_id = 2147483505;
  inline constexpr std::int32_t input_id = 2147483506;
  inline constexpr std::size_t max_client_vdds = 4;

  enum class role_e : std::uint8_t { none, input, monitor, game };
  enum class control_e : std::uint8_t { none, resume, disconnect_monitor, disconnect_input, terminate, monitor, input, running_game };
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
    std::uint64_t generation {};
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
    role_e resume_role {role_e::none};
    bool allowed {};
    bool resume {};
    bool terminate {};
    bool already_complete {};
  };

  struct control_completion_t {
    int status_code {};
    std::string_view status_message;
  };

  enum class terminate_confirmation_e : std::uint8_t { prompt, confirmed };

  [[nodiscard]] bool requires_termination_confirmation(bool terminate_on_first_request, bool caller_owns_active_game);

  [[nodiscard]] terminate_confirmation_e arm_or_confirm_termination(
    std::string_view client_uuid,
    std::uint64_t generation,
    std::int32_t app_id,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()
  );
  [[nodiscard]] std::string_view termination_confirmation_message();
  void clear_termination_confirmation(std::string_view client_uuid);

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

  enum class capture_source_e : std::uint8_t { active_output, exact_output, synthetic_black, invalid };

  struct capture_plan_t {
    capture_source_e source {capture_source_e::active_output};
    std::optional<std::string> output;
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
  [[nodiscard]] std::optional<std::string_view> synthetic_artwork_filename(control_e control);
  [[nodiscard]] projection_t project(const caller_t &caller, const game_t &game, const owner_t &owner, const std::vector<app_t> &configured);
  [[nodiscard]] dispatch_t dispatch(const caller_t &caller, const game_t &game, const owner_t &owner, control_e control);
  [[nodiscard]] bool joins_existing_game_output(role_e role, bool stream_active);
  [[nodiscard]] std::string_view stream_start_response_key(bool launched_from_applist);
  [[nodiscard]] std::optional<control_completion_t> successful_control_completion(control_e control);
  [[nodiscard]] bool input_uses_display_or_audio(role_e role);
  [[nodiscard]] bool uses_audio(role_e role, bool mute_remote_monitor);
  // Remote Monitor keeps host playback enabled independently of the client's
  // localAudioPlayMode request. remote_monitor_mute_audio separately controls
  // whether the monitor receives an audio stream at all.
  [[nodiscard]] bool uses_host_audio(role_e role);
  [[nodiscard]] bool disconnect_monitor_after_stream(bool disconnect_on_stream_end, bool disconnect_on_client_disconnect, bool client_disconnected);
  [[nodiscard]] capture_plan_t capture_plan(role_e role, std::optional<std::string> output = std::nullopt);

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
