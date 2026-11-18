# Tablet posture: manual test plan (Windows host)

Covers `touch_tablet_mode` (see docs/touch-posture.md). The host reports laptop or slate to Windows from client
posture; these tests check each trigger and the Windows result.

## Tools

- Host state: `GET https://<host>:47990/api/tablet-posture` (Bearer API token). Fields: `applied` (what Windows was
  told), `wanted`, `mode` (`desktop`, `tablet` or `handheld`), `gaming` (the Xbox full screen experience is on),
  `override`, `clients[]` with `touch_primary`.
- Simulate the keyboard without a client: `POST /api/tablet-posture` with `{"mode":"slate"}` (detached),
  `{"mode":"laptop"}` (attached), `{"mode":"auto"}` (clients decide again).
- Windows truth, PowerShell on the host:

  ```powershell
  (Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Control\PriorityControl).ConvertibleSlateMode   # 0 slate, 1 laptop
  ```

  Visible result: in a streamed Windows Terminal tap the prompt twice; slate shows the docked touch keyboard and the
  tablet taskbar, laptop shows neither.
- Log lines (sunshine.log): `Tablet posture: host is now slate (tablet)` / `laptop (desktop)`, `Tablet posture: override ...`,
  `Client [..] posture update ...`.
- Linux 2-in-1 client without a keyboard dock: `sudo scripts/sim-tablet-switch.py tablet|laptop|cycle` creates a virtual
  tablet-mode switch; KWin follows it and a posture-aware client reports the change like a real keyboard detach.

## Expected states

| Situation | `ConvertibleSlateMode` | `applied` |
|---|---|---|
| No stream | 1 (laptop) after the 3 s grace | laptop |
| Phone / handheld / tablet class, or paired touch device | 0 | slate |
| `laptop` or `desktop` posture | 1 | laptop |
| Override `slate` / `laptop` | 0 / 1 | slate / laptop |

While slate the values `ConvertibilityEnabled` and `CSMIgnoreDisplayConfig` under `PriorityControl` are 1; afterwards they are
back to what they were (normally absent), and `tablet_posture.journal` in the config folder is gone.

## Tests

1. **Override, no client.** Before: `applied` laptop. POST `slate`: within 2 s `applied` slate, `ConvertibleSlateMode` 0, taskbar
   changes to tablet layout. POST `laptop`: `applied` laptop after the 3 s return grace. POST `auto`: stays laptop with no client.
2. **Phone launch.** Stream Desktop. `clients[]` shows the phone with `touch_primary: true`, `applied` slate, tap a Windows Terminal
   prompt: docked keyboard. Stop the stream: laptop 3 s later, values restored.
3. **Stock client (ROG Ally, Artemis).** Client must be flagged `touch_device` (paired client list). Same as test 2.
4. **Keyboard detach and flip, 2-in-1 client.** Start a stream in laptop posture (`applied` laptop). Detach the keyboard (or run
   `sudo scripts/sim-tablet-switch.py tablet`): the client posts `/posture`, `applied` becomes slate within about 2 s. Attach
   it (or `laptop`): laptop again after 3 s. Repeat flipping three times; the log must show one transition per flip and no error.
   Client option "Reconnect when posture changes" restarts the stream on a size boundary change; the host must still end in the
   right state (the 3 s grace hides the stop/start between the two sessions).
5. **Two clients.** A 2-in-1 (laptop posture) and a phone streaming: slate. Stop the phone: laptop after 3 s while the
   2-in-1 keeps streaming.
6. **Disabled.** `touch_tablet_mode = disabled`, restart the service, stream from the phone: `applied` stays laptop, `enabled`
   false in the API, no registry change.
7. **Crash recovery.** Slate from a stream, kill the host process (`taskkill /f /im sunshine.exe`), start it again: log line
   `restoring the state left by a previous host run`, `ConvertibleSlateMode` 1, declaration values removed, journal gone.
8. **Uninstall.** After uninstalling, `Get-PnpDevice | ? HardwareID -contains PNP0C60` lists no `ROOT\HIDCLASS` node.
9. **Handheld client, Xbox full screen experience.** Host has the Xbox app chosen as home app (Settings > Gaming > Xbox
   mode). Stream from a client that reports `handheld` (a handheld PC, or a phone with a gamepad grip): log
   `Client posture class handheld (handheld mode)`, then `host is now slate (tablet)` and `host is now in the Xbox full
   screen experience`; the stream shows the home app full screen. Stop the stream: the host leaves it and returns to
   laptop after the 3 s grace. `SystemDialogResults` under the user's `GamingConfiguration` key holds no value the host
   added, and `gaming_posture.journal` is gone.
10. **Handheld options.** `handheld_mode = tablet`: the same client gets tablet mode and no Xbox full screen experience.
    `handheld_mode = desktop`: desktop. Per-client `posture_mode` `handheld` on a keyboard client: Xbox full screen
    experience; `tablet`: tablet mode only; `desktop`: desktop.
11. **No home app.** Set the home app to None, stream from a handheld client: warning `the logged-on user has no home
    app`, the client keeps tablet mode, no confirmation appears on the host screen.

## Known limits

- Needs administrator rights for the host service (the HKLM declaration and the device).
- The Xbox full screen experience needs a Windows build that offers it and a home app chosen by the logged-on user.
- The virtual display must be the only active display; a display positioned away from the origin keeps Windows out of tablet posture.
- WPF text boxes (for example in Playnite) do not auto-show the keyboard even in slate; that is the application.
