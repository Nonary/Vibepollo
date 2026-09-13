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
