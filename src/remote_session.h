#pragma once

#include <chrono>
#include <cstdint>
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
    std::string output;
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
    std::string source_address;
    bool encrypted {};
    role_e role {role_e::game};
    std::chrono::steady_clock::time_point expires_at {};
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

  class pending_registry_t {
  public:
    bool add(pending_t pending, std::string *warning = nullptr);
    std::optional<pending_t> match_encrypted(std::string_view client_uuid, std::chrono::steady_clock::time_point now);
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
