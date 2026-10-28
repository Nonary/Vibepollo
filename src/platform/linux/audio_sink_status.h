#pragma once

#include "display_helper_process.h"

#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace platf::audio_sink_status {
  inline constexpr auto query_budget = std::chrono::milliseconds {250};

  struct status_t {
    bool muted;
    bool zero_volume;
  };

  // pactl's JSON inventory uses PulseAudio's raw integer channel volumes.
  // Unknown/malformed state must not be confused with a muted sink.
  inline std::optional<status_t> from_inventory(std::string_view inventory, std::string_view sink) {
    const auto parsed = nlohmann::json::parse(inventory, nullptr, false);
    if (!parsed.is_array()) return std::nullopt;
    for (const auto &entry : parsed) {
      if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string() ||
          entry["name"].get_ref<const std::string &>() != sink) continue;
      if (!entry.contains("mute") || !entry["mute"].is_boolean() ||
          !entry.contains("volume") || !entry["volume"].is_object() || entry["volume"].empty()) {
        return std::nullopt;
      }
      bool zero_volume = true;
      for (const auto &channel : entry["volume"]) {
        if (!channel.is_object() || !channel.contains("value") ||
            !channel["value"].is_number_unsigned()) return std::nullopt;
        zero_volume = zero_volume && channel["value"].get<std::uint64_t>() == 0;
      }
      return status_t {entry["mute"].get<bool>(), zero_volume};
    }
    return std::nullopt;
  }

  inline linux_private_display::helper_process::result_t query_until(
    const std::vector<std::string> &command, std::chrono::steady_clock::time_point deadline) {
    if (command.empty() || std::chrono::steady_clock::now() >= deadline) return {};
    std::vector<const gchar *> argv;
    for (const auto &argument : command) argv.push_back(argument.c_str());
    argv.push_back(nullptr);
    GError *error = nullptr;
    auto *process = g_subprocess_newv(argv.data(),
      static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE), &error);
    if (!process) {
      if (error) g_error_free(error);
      return {};
    }
    // Reuse the existing private-context deadline/cancellation mechanism. A
    // timeout kills this session client; broker disconnect handling cancels
    // and reaps its exact read-only pactl worker. GSubprocess owns local reaping.
    auto result = linux_private_display::helper_process::communicate_until(process, deadline, true);
    g_object_unref(process);
    return result;
  }

  template<class Query, class Warn>
  void inspect_session_capture(Query &&query, Warn &&warn,
    std::chrono::milliseconds budget = query_budget) {
    // Both observations share one absolute budget. Unknown or late status is
    // optional diagnostic data, never a reason to prevent audio capture.
    const auto deadline = std::chrono::steady_clock::now() + budget;
    const auto current = query(std::vector<std::string> {"audio-get-default"}, deadline);
    if (!current.success) return;
    const std::string_view output {current.stdout_text};
    const auto first = output.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return;
    const auto sink = output.substr(first, output.find_last_not_of(" \t\r\n") - first + 1);
    if (std::chrono::steady_clock::now() >= deadline) return;
    const auto inventory = query(std::vector<std::string> {"audio-list-sinks", "details"}, deadline);
    if (!inventory.success) return;
    if (const auto status = from_inventory(inventory.stdout_text, sink);
        status && (status->muted || status->zero_volume)) {
      warn(std::string {sink}, status->muted, status->zero_volume);
    }
  }
}  // namespace platf::audio_sink_status
