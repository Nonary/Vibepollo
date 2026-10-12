# Host-only controller reconnect probe

This standalone Windows utility creates libvirtualgamepad controllers directly,
sends neutral input, and removes them repeatedly. No Moonlight/Artemis client or
physical controller is needed. It targets driver/API/Steam enumeration; it does
not exercise Vibepollo's streaming-session cleanup code.

The installed driver is used unchanged. The probe builds only a client executable,
does not install a driver, and uses the repository's protocol handshake. It stops
on a version mismatch, connection failure, or occupied controller slot.

## Build on the Windows host

Use a checkout with the `third-party/libvirtualgamepad` submodule initialized,
Visual Studio C++ build tools, Windows SDK, and CMake. No WDK build is needed.
From the repository root:

```powershell
cmake -S tools/gamepad_repro -B build/gamepad-repro -A x64
cmake --build build/gamepad-repro --config Release
```

These commands assume CMake's Visual Studio generator. In a Developer Command
Prompt with Ninja, use `-G Ninja` instead of `-A x64`; the executable will be in
`build/gamepad-repro` rather than its `Release` subdirectory.

## First reproduction attempt

Disconnect all streaming sessions, leave Steam running on the host, and open
Steam's controller settings. Keep other controllers unchanged during each run
so baseline comparisons remain useful. From PowerShell:

```powershell
.\build\gamepad-repro\Release\vhf-gamepad-repro.exe --profile both --cycles 20 --cleanup alternate |
    Tee-Object gamepad-repro-both.log
```

`both` creates one Xbox Series controller and one DualSense, approximating the
reported Steam Deck plus paired DualSense inventory. Each cycle leaves them
connected for five seconds, then disconnected for five seconds. Odd cycles
explicitly destroy controllers while keeping the client handle open during the
observation window. Even cycles close the handle without explicit destruction,
exercising the driver's owner-close cleanup. Ctrl+C performs cleanup and stops.

The probe logs elapsed time, present HID interfaces for the five supported native
VID/PIDs, and actual OS XInput slots before testing and at both phases of every
cycle. HID interfaces are not logical controller counts. Physical controllers
and Steam-created virtual controllers can also appear in these inventories.
Unreadable HID paths and API failures are reported, so missing output must not be
treated as proof of removal. Exit zero means the lifecycle operations completed,
not that Steam or every Windows API removed the controllers correctly.

Run individual profiles to identify which is affected:

```powershell
.\build\gamepad-repro\Release\vhf-gamepad-repro.exe --profile xbox --cleanup destroy
.\build\gamepad-repro\Release\vhf-gamepad-repro.exe --profile xbox --cleanup close
.\build\gamepad-repro\Release\vhf-gamepad-repro.exe --profile dualsense --cleanup alternate
```

`--profile ds4` and `--profile switch` are also available. Increase observation
windows with `--connected-ms 15000 --disconnected-ms 15000` if enumeration is slow.
For an arrival/removal stress run, use `--cycles 100 --connected-ms 500
--disconnected-ms 500`, then repeat with longer waits to distinguish transient
enumeration delays from persistent ghosts. A fast run may miss devices entirely.

Repeat the slow test with Steam fully exited, then with Steam started while the
probe's controllers are already connected (use a long connected interval).
Preserve each run's log separately. If investigating PR #3, compare the same
tests with the existing and patched installed driver versions and record them.

## Isolate SDL from Steam

[PR #3's diagnostic directory](https://github.com/Nonary/libvirtualgamepad/tree/a1d760489b0fa58d1842129e2eff916a2657a8e5/driver/tests/gameinput)
contains `probe_sdl_identity.py`. In a separate checkout of that PR, run it before
starting this lifecycle probe. It inventories Steam's actual SDL DLL in its own
process and keeps that process alive across device arrivals and removals:

```powershell
python driver/tests/gameinput/probe_sdl_identity.py 'C:\Program Files (x86)\Steam\SDL3.dll' --gameinput 0 --gameinput-raw 1 --samples 240
```

Repeat with `--gameinput 1 --gameinput-raw 0`, then `--gameinput 0
--gameinput-raw 0`. These settings affect the diagnostic process, not Steam.
The probe disables HIDAPI, DirectInput, WGI, and RawInput to isolate GameInput and
XInput, so it cannot measure DualSense HIDAPI/GameInput duplication. Use actual
Steam for that comparison. Save the SDL version/revision and each run's output.

SDL's [GameInput hint fix](https://github.com/libsdl-org/SDL/commit/c4cfb739ae66dd256ea1c16273a93994a51225db)
is a candidate for Xbox backend duplication, not a demonstrated disconnect fix.
The PR's previous SDL experiments began with devices already connected and did
not reproduce both Xbox entries. Starting the watcher first tests an additional
arrival-order condition; it is not a guaranteed reproduction.

## Interpret the result

- Windows returns to baseline, but Steam retains entries: investigate Steam/SDL
  tracking. Fully restart Steam and compare. A fresh Windows inventory is useful
  corroboration, not proof that every API has cleared its cached state.
- Source controller HID paths remain after a long disconnected interval even
  with Steam exited: investigate driver/PnP removal. Repeated XInput slot numbers
  alone do not establish the identity or owner of a lingering controller.
- Only the SDL probe retains entries while fresh OS inventories clear: this
  narrows the issue to SDL/GameInput; compare with an SDL build containing the fix.
- The host-only probe stays clean, but a real stream leaves controllers behind:
  investigate Vibepollo's session teardown and differences in timing or input.

The driver source/root node and greyed-out historical Device Manager entries
normally remain. Judge currently present controller children and API entries.
Keep two-controller cases in the matrix so deduplication does not hide a real
second controller. This utility itself has not been run against a live Windows
driver in the macOS development environment.
