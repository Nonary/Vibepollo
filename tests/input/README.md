# Input disconnect regression tests

Run from the repository root:

```sh
python3 tests/input/test_delayed_mouse_release.py
python3 tests/input/test_disconnect_release.py
python3 tests/input/test_browser_disconnect.py
python3 tests/input/test_peer_state_bridge.py
```

These scripts compile the production handlers with a deterministic task worker
and recording platform backends. They do not inject input into the test machine.
They cover all keycodes, changed mappings and flags, overlapping clients and
remaps, synthetic modifiers, repeat cancellation, all five mouse buttons,
controller removal and neutralization, touch/pen device lifetime, late packets,
independent browser input contexts sharing capture coordinates, and WebRTC
connection-loss/recovery callbacks.

The scripts do not replace a full platform build or driver-level testing.
The WebRTC change includes the `third-party/libwebrtc` submodule's C bridge;
rebuild that dependency before linking Vibepollo (the cached library needs the
new `lwrtc_peer_register_state_callback` export).

## Windows integration check

Use both Moonlight and browser streaming, including two connected viewers:

1. Hold an ordinary key, then each of Alt/Ctrl/Shift/Win, and abruptly disconnect
   the client. Check that host `GetAsyncKeyState` reports the streamed keys up
   and that typing and Explorer double-click work normally.
2. Repeat with a keybinding remap, including an ordinary key mapped to another
   ordinary key and Alt mapped to Win. Change the mapping while the key is held;
   the originally pressed host key must be released.
3. Hold each mouse button, including X1 and X2, while disconnecting. Test a drag
   using absolute input and a disconnect immediately after releasing left-click.
4. Hold controller buttons, Back (before its Home timer expires), triggers and
   both sticks off-center. Disconnect the stream, and separately unplug one
   controller while another keeps sending packets. Verify neutral state/device
   removal and no delayed Home press. Repeat for ViGEm and VHF.
5. Disconnect during a touch drag, pen contact/hover with barrel buttons, and a
   controller touchpad contact. Verify that contacts and devices are removed.
6. Hold the same key from two clients. Disconnect one; the other must retain its
   key until release. Check that another client's key-up cannot release it.
7. With two browser viewers, disconnect the one holding input while the other
   continues streaming. Verify cleanup, working absolute coordinates in the
   remaining viewer, and no stale controller-slot allocation on reconnect.
8. Drop the browser client's network while holding input without closing its
   tab. Verify release when WebRTC reports disconnection, input recovery after
   reconnection, and session teardown if the peer fails.
9. Repeat disconnect while capture teardown is delayed. Input cleanup should
   run before the capture joins finish. Verify healthy Alt+Tab, typing, mouse,
   controller and pen/touch use after reconnect.

# Steam Controller (2026) host regressions

These harnesses slice the production Steam Controller code out of the host
sources by marker and compile it with fake driver/slot boundaries under
ASan/UBSan. `nlohmann/json` headers are needed (`AUDIT_JSON_INCLUDE` can
override the include directory); `--baseline` reads the production files from
`HEAD` where supported.

`python3 tests/input/test_haptic_queue.py` runs the production
`raise_steam_haptic` forwarding and the gamepad feedback mailbox policy: the
`forward_rumble` switch, stop-key coalescing behind an older effect, per-side
stops surviving a full queue, and every controller's state keys surviving a
sustained haptic burst with a bounded event backlog.

`python3 tests/input/test_steam_settings_forward.py` runs the production
`raise_steam_settings` forwarding: only a `LI_CCAP_STEAM_SETTINGS` client gets
settings writes, a chunked SET_SETTINGS_VALUES write stays whole and in order
around haptic traffic, settings chunks are a reserved class a haptic burst
cannot evict, floods keep the newest sixteen chunks, and all nine triples
reach the client byte for byte.

`python3 tests/input/test_vhf_feedback_retirement.py` runs the production VHF
slot retirement (`vhf_gamepad_t::free`) against a real feedback queue: a
retired controller's queued haptic and settings output is discarded, the other
controller's feedback keeps its FIFO order, and a reused client-relative index
receives only its new messages.

`python3 tests/input/test_vhf_feedback_drain.py` runs the control thread's
feedback drain from `stream.cpp` against the production queue and slot
retirement with a barrier between its readiness check and its dequeue: the
drain must finish when a controller (including the last one) retires its only
queued message in that window, when the queue stops, and it must still deliver
the surviving controller's messages in order. `--baseline` compiles `HEAD`.

`python3 tests/input/test_vhf_rumble_admission.py` runs the production
`dispatch_rumble_poll` with the protected admission against a mailbox filled to
capacity with every controller's state keys and the reserved settings chunks:
the plain raise is refused there, the active-to-zero rumble stop is admitted and
replaces the stale motor state, and a stopped queue reports rejection.

`python3 tests/input/test_legacy_feedback_admission.py` runs the production ViGEm
rumble/LED callbacks (`vigem_t::rumble`, `vigem_t::set_rgb_led`) and the inputtino
rumble/LED lambdas against the shared mailbox under mixed-controller pressure:
legacy producers coalesce to one pending entry per state, a legacy zero stop and
a Steam slot's synthesized expiry stop are both admitted at capacity, a refused
message leaves the duplicate cache stale for retry, and duplicates are suppressed.
`--baseline` compiles `HEAD`.

`python3 tests/input/test_usbip_trigger_admission.py` runs the production DualSense
USB/IP flush (`flush_output`, `publish_latest`) against a mailbox nobody drains:
left-only and right-only trigger programs both stay queued, a newer program for
the same enables replaces its predecessor, hundreds of changed programs coalesce
to one per enable set, a Steam slot's consumed-start expiry stop is still
admitted afterwards, and a refused program keeps its enables pending.

`python3 tests/input/test_vhf_fallback.py` executes the production Windows
allocation and profile-selection functions with fake VHF/ViGEm driver objects:
a Steam Controller client under `auto` prefers the VHF driver, a client-derived
profile the driver lacks retries with the automatic profile under `auto`/`vhf`,
an explicit `vhf_*` profile is never substituted, and controller touch is
advertised for every selection that can yield a touchpad.

`python3 tests/input/test_sc26_buttons.py` compiles the production VHF input
adapter together with the driver's real Steam Controller encoder from the
`third-party/libvirtualgamepad` submodule and checks all sixteen physical
pad/trigger click combinations and their releases, the capability/profile
gating of the `sc26_buttons_explicit` metadata bit, and the legacy fallbacks.
