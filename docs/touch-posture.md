# Touch posture and virtual display size (contract v2)

Windows has no tablet-mode switch. It docks the touch keyboard from hardware
signals, and the one a headless host can control is the physical size reported by
the monitor. A virtual display that reports 600 x 340 mm (27 in) makes Windows
float the touch keyboard over a touch client's stream. This host extension lets a
touch client (phone, handheld, tablet or 2-in-1 in tablet posture) get a virtual
display that reports a small physical size instead.

The reported size depends only on the device type and the keyboard or posture
state. It never depends on the client's screen size, DPI or aspect ratio, and it
never reads a physical monitor's EDID. The stream resolution is unchanged; only
the reported millimetres differ.

Clients that send nothing, and paired clients that are not flagged as touch
devices, get exactly the display they got before.

Version 1 of this contract derived the size from the client's diagonal or DPI. That
is gone: hosts still accept `clientDiagonalMm` and `clientDpi` from older clients
without error, but ignore them completely.

## Launch and resume arguments

All arguments are optional query arguments of `/launch` and `/resume`. Unknown
arguments were always ignored by hosts, so older hosts and clients keep working.

| Argument | Values | Meaning |
|---|---|---|
| `clientPosture` | `desktop`, `laptop`, `tablet`, `handheld`, `phone` | Client class. An unknown value discards all posture arguments. |
| `clientTouch` | `0` or `1` | Informational. With no `clientPosture`, `1` is treated as `tablet`. |
| `clientDiagonalMm`, `clientDpi` | any | Sent by older clients. Accepted and ignored. |

`phone`, `handheld` and `tablet` all mean "touch-primary, no physical keyboard in
use": the docked touch keyboard is wanted. `laptop` and `desktop` mean a physical
keyboard is in use or the device is not touch: the host keeps its default size.
A `clientTouch` that is not `0` or `1` is ignored.

## Size policy

The policy applies for `tablet`, `handheld` and `phone`, for `clientTouch=1` with
no `clientPosture`, and for a paired client flagged as a touch device that sent no
posture. An explicit `desktop` or `laptop` posture wins over the flag. Otherwise no
size is requested and the driver keeps its default.

1. Every such client gets one fixed diagonal: `touch_posture_diagonal_mm` from
   `sunshine.conf`, default 356 mm (14 in), valid range 100..419 mm. A value
   outside the range, or not a plain integer, is ignored with a warning in the log
   and the default is used. There are no per-class defaults: phone, handheld and
   tablet are the same.
2. The stream pixels only split that diagonal into width and height, assuming square
   pixels (16:9 if the stream size is unknown or not positive). A portrait stream
   reports the landscape size turned, never rescaled.
3. Width and height are rounded to whole millimetres, each at least 10 mm.

So the size is the same whatever the client is. A phone and a 65 in TV client that
both send `clientPosture=tablet` or `clientTouch=1` get the same 14 in (356 mm)
display. At a 16:9 stream such as 1920x1080 or 3840x2160 that is 310 x 175 mm for
both; at a 20:9 stream (2400x1080) it is 325 x 146 mm. Only the stream aspect
changes the split, never the diagonal. The same holds for any stream resolution,
including the arbitrary upscaled resolutions newer clients render at.

The driver writes the size to EDID bytes 0x15 and 0x16 in whole centimetres (round
half up, at least 1) and to the preferred detailed timing in millimetres. The
policy is tested to keep the centimetre-rounded diagonal below 17.0 in (43.2 cm) for
every resolution and aspect from 1:64 to 64:1: a 419 mm diagonal encodes at most
426 mm, and the highest configurable value that could still stay below 17.0 in is
424 mm.

### Why 18 in is the docking limit

Windows 11 build 26300 docks the touch keyboard or floats it depending on the
reported physical size alone, with no touch hardware or slate mode involved. Sizes up
to 38 x 21 cm (43.4 cm, 17.1 in) dock and show the dock icon. 40 x 22 cm (45.7 cm,
18.0 in) and larger float. The edge therefore lies between 17.1 and 18.0 in. The
host stays below 17.0 in, so the default of 14 in has a wide margin and the 16.5 in
maximum a small one. This is observed behaviour, not documented by Microsoft, and
may differ on other Windows builds.

The size is passed as `physical_width_mm` and `physical_height_mm` when the bundled
virtual display driver creates the display. The driver takes millimetres as is: it
does not rescale or swap them for a portrait stream. The log shows `Posture:
requesting virtual display physical size` with the diagonal in mm. SudoVDA has a
static EDID and no size field, so it cannot apply the size; the host logs a warning
and keeps its fixed monitor size. Windows reads the monitor size once when the
monitor arrives, so a running display cannot be resized.

## Touch device flag for stock clients

Clients that cannot send a posture (for example an unmodified Moonlight on Android)
can be marked per paired client. A flagged client that sends no posture is treated
as touch-primary and gets the same fixed diagonal as a `tablet`.

- Web UI: Devices, edit device, "Treat this device as a touch device".
- API: `POST /api/clients/update` with `{"uuid":"<uuid>","touch_device":true}`. The
  response reports `touch_device_updated`. `GET /api/clients/list` returns
  `touch_device` for every client.
- Stored in the paired device record as `touch_device` in `sunshine_state.json`, only
  when set.

The flag applies the next time the virtual display is created.

## Resume

A resume whose posture crosses the touch boundary ("no size" versus "a size", for
example tablet to laptop when a keyboard is attached) recreates the client's virtual
display before streaming resumes, so the new size takes effect. A change between two
touch classes (for example phone to tablet) is not a change at all, because they get
the same size. The display is not recreated when the history of the display is
unknown, for shared virtual displays, or when the request joins another client's
running stream.

## Runtime update: `/posture`

A client can report a posture change while streaming on the same HTTPS port, with
the paired client certificate, like `/bitrate`.

```
GET|POST /posture?clientPosture=tablet&clientTouch=1
```

The arguments are the same as above and replace the client's stored posture. At
least one valid `clientPosture` or `clientTouch` is required; a call that carries
only the legacy `clientDiagonalMm` or `clientDpi` is answered with 400. The caller
needs the "View stream" permission and an active session. Calls closer than one second apart per client are rejected.

| Result | XML |
|---|---|
| Accepted | `<root status_code="200"/>` |
| No valid posture argument | `status_code="400"` |
| No View stream permission, unknown certificate | `status_code="403"` |
| No active session for the client | `status_code="404"` |
| Closer than one second to the previous call | `status_code="429"` |

The response also carries `display_change`: `1` when the display created at launch or resume no longer
fits the latest state (the client became or stopped being touch-capable), else `0`. It returns to `0` when
the client goes back to its launch state. A client should restart its stream only
on `1`; clients that ignore the attribute keep working.

The display size follows touch *capability* (a tablet, handheld or phone class, `clientTouch=1`, or the
paired-client flag), not the keyboard state. A 2-in-1 that launches as `laptop` with `clientTouch=1` already
has the touch-sized display, so attaching, detaching or flipping its keyboard only switches the host between
desktop and tablet mode: no display replacement and no stream restart. The running display is never recreated
by this call; a change that does need one applies at the next launch or resume.

## Tablet mode (Windows)

Windows shows its touch keyboard for text fields, docks it, and switches the taskbar to its
tablet layout only in *tablet posture*. `InputHost` (`DevicePostureHelpers::CalculateDevicePostureMode`)
decides it: the session is the console session, the device is a convertible
(`PriorityControl\ConvertibilityEnabled`), the laptop/slate indicator says slate
(`PriorityControl\ConvertibleSlateMode` = 0), it is not docked, and the display configuration is a valid
tablet one (an internal panel, or `PriorityControl\CSMIgnoreDisplayConfig`, and no display away from the
origin). A real touch screen does not matter; the real keyboard does not matter either.

A streaming host is none of that, so with `touch_tablet_mode` the host reports it itself, using the same
mechanism a convertible laptop uses when its keyboard is folded back or detached:

- Microsoft's inbox GPIO Laptop or Slate Indicator Driver (`msgpiowin32.inf`, hardware id `PNP0C60`).
  The host creates the root-enumerated device on first use and writes one byte to its interface:
  `0` = slate, `1` = laptop. The kernel updates the posture live; no restart.
- While the host is a slate it also sets the two OEM declarations above, and restores their previous values
  afterwards. A journal file in the config folder restores them at the next start if the host stopped abnormally.

Who drives it:

| Event | Host state |
|---|---|
| Session starts with a touch-primary client (tablet, handheld, phone, touch device) | slate |
| Session starts with `laptop` or `desktop` posture | laptop |
| `/posture` changes the class of a streaming client (keyboard attached, detached, flipped, flipped back) | follows it, live |
| Several clients stream | slate while any one is touch-primary |
| Last touch-primary session ends, or the host stops | laptop |

Needs the host service to run with administrator rights and the virtual display to be the only active
display (a display positioned away from the origin keeps Windows out of tablet posture).

## Host modes and options

The host has three modes per client:

| Mode | Windows |
|---|---|
| desktop | Laptop indicator: normal desktop behaviour. |
| tablet | Slate indicator: tablet posture. |
| handheld | Slate indicator, plus the Xbox full screen experience ("gaming posture"). |

Which mode a client gets, from its state (`effective_class`):

| Client | Mode |
|---|---|
| `desktop` or `laptop` posture (keyboard attached), or no posture and no touch | desktop |
| `tablet`, `phone`, `clientTouch=1`, or a paired touch device | tablet |
| `handheld` | set by `handheld_mode` (below) |

- `handheld_mode` (host option): `xbox_fullscreen` (default) makes handheld clients handheld, `tablet` treats them
  like tablets, `desktop` treats them like keyboard-attached desktops. Where Windows cannot enter the Xbox full
  screen experience (see below) a handheld client gets tablet mode.
- `posture_mode` (per paired client, set in the web UI as "Host mode for this device" or through
  `POST /api/clients/update`): `auto` (default) follows the client's own posture; `desktop`, `tablet` and
  `handheld` force that mode for the client. The override applies at launch/resume and to `/posture` updates.
  A forced `desktop` on a touch-capable client keeps its touch-sized virtual display.
- With several clients the strongest mode wins: handheld, then tablet, then desktop.
- `touch_tablet_mode = disabled` turns all of this off: the host never reports a slate or the gaming posture.

## Xbox full screen experience (Windows)

Windows calls its Xbox full screen experience ("Xbox mode") the *gaming posture*. In handheld mode the host
enters it right after the slate switch and leaves it, after the same 3 s grace, before it goes back to laptop.

What Windows needs before anything can enter it:

- A build that offers it (`IsGamingFullScreenExperienceSupported`). The host never enables it on a build that does not.
- A home app for the logged-on user: Settings > Gaming > Xbox mode > Choose home app, normally the Xbox app
  (stored as `GamingHomeApp` under the user's `GamingConfiguration` key). Without one Windows offers no switch.

How the host switches:

- Windows grants `SetGamingFullScreenExperience` only to a process that runs as the logged-on user; a service
  account is refused, also while impersonating, and is told the experience is inactive. The host service
  therefore starts its own executable as the logged-on user with `--gaming-posture-switch enter|leave`. That
  instance does no host initialization: it reads the state, switches only when the state differs, waits for
  Windows to confirm and returns the result as its exit code. A host that already runs as the user calls the
  switch directly. No keyboard shortcut is sent.
- Windows asks for confirmation when entering and leaving, and remembers the answers per user under
  `GamingConfiguration\SystemDialogResults`. While it switches, the host supplies the answer "switch now, no
  restart" for the confirmations that have none, and removes exactly those afterwards; an answer the user gave
  is never touched. A journal file in the config folder removes leftovers at the next start if the host
  stopped abnormally.

When the switch does not happen (no home app, an older build, a confirmation the host does not know) the log
says why and the client keeps tablet mode. The host does not retry until the wanted mode changes.

Log lines: `Tablet posture: host is now in the Xbox full screen experience`,
`Gaming posture: Windows cannot enter the Xbox full screen experience: the logged-on user has no home app. ...`,
`Tablet posture: could not enter the Xbox full screen experience; staying in tablet mode`.

## Status

`GET /api/clients/list` adds a `posture` object to every client that is streaming:

| Field | Meaning |
|---|---|
| `posture` | Class the client declared, or `null`. |
| `class` | Class the size policy applies, or `null` for no size. |
| `touch_device` | The paired-client flag. |
| `wanted_size` | `{width_mm, height_mm}` the policy wants for the latest hints and the host diagonal, or `null`. |
| `applied_known` | Whether the host knows what the display was created with. |
| `applied_size` | Size the display was created with; `null` is the driver default or unknown. |
| `update_pending` | A runtime update changed the wanted size after the display was created. |

The log lines `Client posture class ...`, `Posture: requesting virtual display
physical size ...` (both with the diagonal in mm) and `Posture: resume crosses the
touch size boundary` show each step.

## Limits

- Only the bundled virtual display driver can apply a size.
- WebRTC sessions and Remote Monitor do not read the posture arguments. When they
  create a display with the same identity as an earlier touch session they reuse that
  session's size.
- Windows may cache a monitor's description per device instance. Check the reported
  size with `WmiMonitorBasicDisplayParams` after the first touch session.
