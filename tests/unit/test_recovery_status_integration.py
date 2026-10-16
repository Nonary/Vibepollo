"""Source integration checks for the portable recovery status policy."""
import pathlib
import sys


root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
legacy = (root / "tools/display_settings_helper.cpp").read_text()
v2_ipc = (root / "tools/display_settings_helper_v2.cpp").read_text()
v2_fsm = (root / "src/platform/windows/display_helper_v2/state_machine.cpp").read_text()
v2_pump = (root / "src/platform/windows/display_helper_v2/win_event_pump.cpp").read_text()
v2_pump_header = (root / "src/platform/windows/display_helper_v2/win_event_pump.h").read_text()
v2_runtime = (root / "src/platform/windows/display_helper_v2/runtime_support.h").read_text()

assert "RecoveryStatus = 15" in legacy and "RecoveryStatusResult = 16" in legacy
assert 'j.find("sunshine_restore_ticket")' in legacy
assert "state.restore_poll_thread.join()" in legacy
assert "recovery_status.query(" in legacy
assert "recovery_status.observe_event()" in legacy
assert "if (!restore_attempt_completed)" in legacy, "terminal failure must retain the read-only event observer"
assert "HWND_MESSAGE" not in legacy, "WM_DISPLAYCHANGE needs the hidden top-level observer window"
assert "monitor_power_edge_policy" in v2_pump_header
assert "notify_recovery_status_locked();" in legacy
assert legacy.index("state.recovery_notification = {};") < legacy.index("async_pipe.stop();")

assert "RecoveryStatus = 15" in v2_ipc and "RecoveryStatusResult = 16" in v2_ipc
assert "RecoveryStatusCommand" in v2_ipc
assert 'j.find("sunshine_restore_ticket")' in v2_ipc
assert "handle_recovery_status_command" in v2_fsm
assert "recovery_status_policy_.query(" in v2_fsm
assert "recovery_status_policy_.parked()" in v2_fsm
assert "notify_recovery_status();" in v2_fsm
assert "recovery_status_result_callback_(ticket, status, event, false, epoch);" in v2_fsm
assert "scheduler_.disarm();" in v2_fsm
assert "record_liveness_ping()" in v2_fsm and "record_liveness_ping()" in v2_runtime
assert "parked ? 1u : 0u" in v2_ipc
assert "response.push_back(parked_ack ? 1u : 0u)" in legacy
liveness_ping = v2_runtime.split("void record_liveness_ping()", 1)[1].split("bool check_timeout()", 1)[0]
assert "recovery_deadline_.reset();" not in liveness_ping
