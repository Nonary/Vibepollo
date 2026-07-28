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
    VirtualDisplayMonitoring,  // Monitors virtual display for crashes during active session
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

  /// The semantic scope of a configuration target. An omitted public device
  /// id means the original primary duplicate group, not an arbitrary single
  /// monitor selected after Windows reorders its topology.
  enum class DeviceTargetKind {
    None,
    ExplicitDevice,
    DefaultPrimaryGroup,
  };

  /// Pre-activation candidates used only to determine whether Windows kept a
  /// usable target after SetDisplayConfig adjusted a requested topology.
  struct TopologyActivationTarget {
    DeviceTargetKind kind = DeviceTargetKind::None;
    std::set<std::string> acceptable_device_ids;
  };

  /// Concrete target context derived from the topology Windows actually
  /// accepted. The original request remains unchanged; this context carries
  /// the duplicate-group verification/mode/HDR scope separately.
  struct ResolvedConfigurationTarget {
    DeviceTargetKind kind = DeviceTargetKind::None;
    std::string representative_device_id;
    std::set<std::string> duplicate_device_ids;
  };

  struct ApplyTopologyPlan {
    ActiveTopology topology;
    /// Candidate target(s) that must survive a Windows-adjusted activation.
    /// For an empty request id this preserves the original primary duplicate
    /// group instead of collapsing it to one arbitrary member.
    TopologyActivationTarget activation_target;
  };

  struct ApplyRequest {
    std::optional<SingleDisplayConfiguration> configuration;
    std::optional<ActiveTopology> topology;
    std::vector<std::pair<std::string, display_device::Point>> monitor_positions;
    /// Restore physical monitor refresh rates (device_id -> num/den) after virtual
    /// display creation resets them.
    std::vector<std::pair<std::string, std::pair<unsigned int, unsigned int>>> refresh_rate_overrides;
    bool hdr_blank = false;
    bool prefer_golden_first = false;
    /// When false, a broken Sunshine connection must not autonomously restore
    /// (stream is intentionally pause-retained).
    bool restore_on_disconnect = true;
    std::optional<std::string> virtual_layout;
    /// Optional snapshot exclusions supplied with this APPLY. Keeping this on
    /// the queued command ensures the state machine, rather than the pipe
    /// reader thread, owns mutation of the restore baseline policy.
    std::optional<std::vector<std::string>> snapshot_exclusions;
    /// Client-generated v2 IPC token echoed in APPLY and verification replies.
    /// This keeps a late completion from a superseded transaction from opening
    /// a newer stream's capture gate.
    std::uint64_t request_id = 0;
  };

  struct SnapshotCommandPayload {
    std::vector<std::string> exclude_devices;
    /// True when the payload carried an exclusion list (even an empty one, which clears).
    bool update_exclusions = false;
  };

  struct ApplyCommand {
    ApplyRequest request;
    std::uint64_t generation = 0;
    /// Connection that owns APPLY/verification replies. Zero is used by
    /// in-process tests and non-IPC callers.
    std::uint64_t connection_epoch = 0;
  };

  struct RevertCommand {
    std::uint64_t generation = 0;
    /// IPC connection that issued this command; zero is autonomous/restore
    /// mode and is intentionally not connection-scoped.
    std::uint64_t connection_epoch = 0;
    /// Prefer golden over previous when the current session snapshot is unavailable.
    bool prefer_golden_if_current_missing = true;
    /// Optional override of the golden-first strategy carried in the REVERT payload.
    std::optional<bool> always_restore_from_golden;
    /// Skip the 5s grace window before the first restore attempt (--restore mode).
    bool immediate = false;
    /// True when triggered by a broken connection / heartbeat loss rather than an
    /// explicit client REVERT; honors the restore-on-disconnect policy.
    bool from_disconnect = false;
  };

  struct DisarmCommand {
    std::uint64_t generation = 0;
    std::uint64_t connection_epoch = 0;
  };

  struct ExportGoldenCommand {
    SnapshotCommandPayload payload;
    std::uint64_t generation = 0;
    std::uint64_t connection_epoch = 0;
  };

  struct SnapshotCurrentCommand {
    SnapshotCommandPayload payload;
    std::uint64_t generation = 0;
    std::uint64_t connection_epoch = 0;
    /// Non-zero for the correlated v2 SnapshotResult wire format.
    std::uint64_t request_id = 0;
  };

  struct RefreshRateCommand {
    std::string device_id;
    unsigned int numerator = 0;
    unsigned int denominator = 0;
    std::uint64_t generation = 0;
    std::uint64_t connection_epoch = 0;
    /// Non-zero for the correlated v2 RefreshRateResult wire format.
    std::uint64_t request_id = 0;
  };

  struct ResetCommand {
    std::uint64_t generation = 0;
    std::uint64_t connection_epoch = 0;
  };

  struct PingCommand {
    std::uint64_t generation = 0;
    std::uint64_t connection_epoch = 0;
  };

  struct StopCommand {
    std::uint64_t generation = 0;
    std::uint64_t connection_epoch = 0;
  };

  struct ApplyCompleted {
    ApplyStatus status = ApplyStatus::Fatal;
    std::optional<ActiveTopology> expected_topology;
    std::optional<ResolvedConfigurationTarget> resolved_target;
    bool virtual_display_requested = false;
    bool display_may_have_changed = false;
    bool durable_recovery_armed = false;
    bool staged_state_prepared = false;
    std::uint64_t generation = 0;
  };

  /// Why a read-only verification was scheduled. Keeping this explicit avoids
  /// conflating the response gate, ordinary settling checks, and the separate
  /// verify-before-reapply sequence used after a recent pipe disconnect.
  enum class VerificationPurpose {
    Initial,
    Stabilization,
    TransientDisconnect,
  };

  struct VerificationCompleted {
    bool success = false;
    VerificationPurpose purpose = VerificationPurpose::Initial;
    std::uint64_t generation = 0;
  };

  struct ResetCompleted {
    bool success = false;
    std::uint64_t generation = 0;
  };

  struct RefreshRateCompleted {
    bool success = false;
    std::string device_id;
    unsigned int numerator = 0;
    unsigned int denominator = 0;
    std::uint64_t generation = 0;
    std::uint64_t connection_epoch = 0;
    std::uint64_t request_id = 0;
  };

  struct RecoveryCompleted {
    bool success = false;
    std::optional<Snapshot> snapshot;
    bool display_may_have_changed = false;
    bool staged_state_reset_attempted = false;
    bool staged_state_reset_succeeded = false;
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
    /// Connection that was live when the watchdog fired. Zero denotes an
    /// autonomous/restore-mode event and is intentionally not connection
    /// scoped.
    std::uint64_t connection_epoch = 0;
  };

  using Message = std::variant<
    ApplyCommand,
    RevertCommand,
    DisarmCommand,
    ExportGoldenCommand,
    SnapshotCurrentCommand,
    RefreshRateCommand,
    ResetCommand,
    PingCommand,
    StopCommand,
    ApplyCompleted,
    VerificationCompleted,
    ResetCompleted,
    RefreshRateCompleted,
    RecoveryCompleted,
    RecoveryValidationCompleted,
    DisplayEventMessage,
    HelperEventMessage>;
}  // namespace display_helper::v2
