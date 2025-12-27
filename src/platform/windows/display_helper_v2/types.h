#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <display_device/json.h>
#include <display_device/types.h>
#include <display_device/windows/types.h>

namespace display_helper::v2 {
  enum class ApplyAction {
    Apply,
    Revert,
    Disarm,
    ExportGolden,
    SnapshotCurrent,
    Reset,
    Ping,
    Stop,
  };

  enum class ApplyStatus {
    Ok,
    HelperUnavailable,
    InvalidRequest,
    VerificationFailed,
    NeedsVirtualDisplayReset,
    Retryable,
    Fatal,
  };

  enum class SnapshotTier {
    Current,
    Previous,
    Golden,
  };

  enum class PolicyDecision {
    Proceed,
    Retry,
    ResetVirtualDisplay,
    SkipToNextTier,
  };

  enum class WatchdogStatus {
    Healthy,
    MissedPing,
    TimedOut,
  };

  enum class State {
    Waiting,
    InProgress,
    Verification,
    Recovery,
    RecoveryValidation,
    EventLoop,
  };

  enum class DisplayEvent {
    DisplayChange,
    PowerResume,
    DeviceArrival,
    DeviceRemoval,
  };

  enum class HelperEvent {
    HeartbeatTimeout,
  };

  using ActiveTopology = display_device::ActiveTopology;
  using EnumeratedDeviceList = display_device::EnumeratedDeviceList;
  using Snapshot = display_device::DisplaySettingsSnapshot;
  using SingleDisplayConfiguration = display_device::SingleDisplayConfiguration;

  struct ApplyRequest {
    std::optional<SingleDisplayConfiguration> configuration;
    std::optional<ActiveTopology> topology;
    std::vector<std::pair<std::string, display_device::Point>> monitor_positions;
    bool hdr_blank = false;
    bool prefer_golden_first = false;
    std::optional<std::string> virtual_layout;
  };

  struct SnapshotCommandPayload {
    std::vector<std::string> exclude_devices;
  };

  struct ApplyCommand {
    ApplyRequest request;
    std::uint64_t generation = 0;
  };

  struct RevertCommand {
    std::uint64_t generation = 0;
  };

  struct DisarmCommand {
    std::uint64_t generation = 0;
  };

  struct ExportGoldenCommand {
    SnapshotCommandPayload payload;
    std::uint64_t generation = 0;
  };

  struct SnapshotCurrentCommand {
    SnapshotCommandPayload payload;
    std::uint64_t generation = 0;
  };

  struct ResetCommand {
    std::uint64_t generation = 0;
  };

  struct PingCommand {
    std::uint64_t generation = 0;
  };

  struct StopCommand {
    std::uint64_t generation = 0;
  };

  struct ApplyCompleted {
    ApplyStatus status = ApplyStatus::Fatal;
    std::optional<ActiveTopology> expected_topology;
    bool virtual_display_requested = false;
    std::uint64_t generation = 0;
  };

  struct VerificationCompleted {
    bool success = false;
    std::uint64_t generation = 0;
  };

  struct RecoveryCompleted {
    bool success = false;
    std::optional<Snapshot> snapshot;
    std::uint64_t generation = 0;
  };

  struct RecoveryValidationCompleted {
    bool success = false;
    std::uint64_t generation = 0;
  };

  struct DisplayEventMessage {
    DisplayEvent event = DisplayEvent::DisplayChange;
    std::uint64_t generation = 0;
  };

  struct HelperEventMessage {
    HelperEvent event = HelperEvent::HeartbeatTimeout;
    std::uint64_t generation = 0;
  };

  using Message = std::variant<
    ApplyCommand,
    RevertCommand,
    DisarmCommand,
    ExportGoldenCommand,
    SnapshotCurrentCommand,
    ResetCommand,
    PingCommand,
    StopCommand,
    ApplyCompleted,
    VerificationCompleted,
    RecoveryCompleted,
    RecoveryValidationCompleted,
    DisplayEventMessage,
    HelperEventMessage>;
}  // namespace display_helper::v2
