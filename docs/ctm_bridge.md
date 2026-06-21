# CTM Bridge integration

Vibepollo can natively supervise the [CTM-USBIP](https://github.com/CTM-Bridge/CTM-USBIP)
agent (`ctm-usbip.exe`) so it no longer needs a separate Windows autostart
(scheduled task). The agent presents a real DualSense/controller — connected to
your Moonlight device — to games natively over USB/IP.

## Design: out-of-tree, atomic against upstream

CTM-USBIP currently ships without a license, so **no CTM source is vendored** into
Vibepollo. The agent is treated as an **opaque prebuilt binary**:

- Vibepollo only *launches and supervises* `ctm-usbip.exe`; it never compiles or
  links any CTM code.
- All Vibepollo-side logic is confined to:
  - `src/platform/windows/ctm_bridge.{h,cpp}` — the lifetime supervisor.
  - `config::ctm` in `src/config.{h,cpp}` — the settings (`ctm_*` keys).
  - `src/main.cpp` — two call sites (`ctm_bridge::start_watchdog()` /
    `stop_watchdog()`).
  - the web UI (`Inputs.vue` + `configFieldSchema.ts` + `stores/config.ts` +
    locale strings).
  - optional packaging in `cmake/packaging/windows.cmake`.

**Upgrading to a newer upstream CTM-USBIP is a drop-in replacement of the exe** —
no source merge, no rebuild of Vibepollo. The only contract we depend on is the
agent CLI `ctm-usbip.exe agent <port> [--enet]`; if upstream changes that, adjust
`build_args()` in `ctm_bridge.cpp` (a single function).

## Runtime behavior

A single supervisor `std::jthread` (started near the Playnite integration in
`main()`, stopped before CRT teardown) ticks every ~5s:

- while `ctm_enable` is true it (re)starts the agent via `ProcessHandler`
  (idempotent: a no-op while alive, a relaunch after a crash);
- while `ctm_enable` is false it terminates any managed instance.

`ProcessHandler` runs the agent with its own folder as the working directory and,
when Vibepollo runs as SYSTEM, launches it into the active user session.

## Configuration (`sunshine.conf` / web UI → Inputs tab)

| Key | Default | Meaning |
|---|---|---|
| `ctm_enable` | `false` | Master switch. |
| `ctm_path` | *(empty)* | Path to `ctm-usbip.exe`. Empty ⇒ `<install>/tools/ctm/ctm-usbip.exe`. |
| `ctm_port` | `48054` | TCP/UDP control + discovery port. |
| `ctm_enet` | `true` | Pass `--enet` (low-latency input transport). |

## Bundling the agent (optional)

The agent ships as a self-contained folder (its own ffmpeg DLLs, `maps/`,
`profiles/`). To bundle it into the installer under `tools/ctm/`, point a CMake
cache var at an unpacked CTM-USBIP release before configuring:

```
cmake -B build -DSUNSHINE_CTM_AGENT_DIR=C:/path/to/CTM-USBIP/Release ...
```

When unset, packaging proceeds without it and users can instead set `ctm_path` to
an external install.
