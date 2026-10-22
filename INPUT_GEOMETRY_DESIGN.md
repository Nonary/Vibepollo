# Vibepollo #301: versioned input geometry integration

**Target base:** `0883672ffb75478f9f444d447ed55de6ecd309cf` (fetched remote `published/master` before this private issue #301 commit)
**Adapted source:** `4a99508ac694178055f9cd51c4b960d378f305dc`, based on fetched source `published/master` ref `84678f6f3f31f8bc45d1cef030144dad7aedb551`. The divergent `4616f7812bad5f3860a90fd89fed45ce7e76184d` callback-stall diagnostic commit is not an ancestor or dependency and is not carried into this adaptation.
**Status:** implemented in this isolated target candidate. The 70-test portable suite passes. Windows production compilation/runtime and the reported freeze remain unverified; this corrects input mapping and transport only.

## Source gap and preserved policy

The WGC output monitor verifies the captured adapter/output identity before treating a virtual-desktop bounds change as input-only. A changed target, invalid/closed capture, observation timeout, or unrepresentable mapping still requests reinitialization; geometry publication does not bypass those paths.

Before this prototype, a verified bounds change updated only a worker-local policy value. `display_t` kept its startup offset and virtual-desktop extent, `video.cpp::make_port()` published no geometry version, and the input worker learned about a port only while translating a client packet. Windows synthetic touch and pen repeat on a timer, so raising a replacement port alone would not serialize cancellation with old-map repeats.

## Prototype transport

- `display_output_monitor_t` owns a synchronized snapshot store containing capture generation, geometry sequence, validity, the verified target identity, target offset and virtual-desktop extent. The store serializes publication, retry invalidation and generation termination without changing ordinary `display_t` fields from the monitor thread.
- A WGC monitor starts with its input mapping invalid and performs an immediate target/bounds observation. The initial DXGI output selection is not enough to open absolute input; only a same-target sample with stable, representable desktop metrics publishes the first valid mapping. WGC also periodically rechecks full output identity even when color-change events are subscribed, covering target changes inside an unchanged whole-desktop envelope. Desktop Duplication retains its startup behavior.
- The full per-session input port copies that version and replaces only the four absolute-mapping fields. Stream dimensions, client letterbox/scaling, logical dimensions, output name and other session state remain intact.
- Sync capture and both async encoder loops compare snapshot versions at loop/frame boundaries and raise a complete input-port update. The async paths check at loop entry and after their bounded image wait.
- Port events reject older capture-generation/geometry-sequence values under the event lock. This prevents a delayed encoder publisher from overwriting a newer snapshot after the input worker has consumed it.
- The input worker tracks the touch-port event generation and reads updates with `view_if_newer()`. When a new event is published, it waits for the event lock rather than skipping the snapshot on a failed nonblocking try-lock. A weak 50 ms timer schedules the same coalesced `input_queue_task_scheduled` worker path even when no client packet arrives. The worker cancels touch and pen state before installing a newer map. An invalid version preserves the last coordinates but marks absolute input unavailable.
- Windows touch and pen repeat callbacks use a generation fence and shared control block. Cancellation waits out an in-flight callback, rejects a dequeued stale callback, and teardown nulls the owner before the raw client context and pointer devices are destroyed.
- Contacts already tracked by the input worker are quarantined after cancellation. Their later MOVE/UP packets are suppressed; a DOWN observed after the transition starts a contact under the new map. Equal versions are no-ops and older versions are rejected.
- If `sample_output()` cannot verify the selected target on the WGC path, the monitor invalidates the current mapping before starting its bounded retry, even when whole-desktop metrics compare unchanged. The target may have moved or been replaced inside the same desktop envelope. Absolute input stays unavailable until a verified mapping is published or the existing timeout requests reinitialization. Desktop Duplication keeps its existing observation policy.
- If a sampled target no longer matches the captured adapter/output identity, WGC invalidates its current mapping before requesting capture reinitialization. This prevents the old map from remaining valid in the capture-owned snapshot while restart is pending. Desktop Duplication keeps its existing behavior.
- Capture exit and `prepare_for_reinit()` terminalize that capture generation as well as invalidating its map. A delayed monitor publication cannot restore validity under the old `diagnostic_id_`; a replacement display gets a new generation and publisher.
- Target-identity changes, closed/invalid capture results, and observation timeout continue to use the existing reinitialization path.
- When a display's `capture()` call returns, its capture-lifetime guard terminalizes the geometry generation before control returns to `video.cpp`. For an effective `capture_e::reinit`, the shared capture owner raises the reinit gate before draining contexts, preserving the existing ordered-reinit path. Every capture return atomically drains queued contexts and publishes the terminal (invalid) port to every live active or newly drained per-session input event before releasing the old display. Non-reinit returns do not raise the reinit signal, so ordinary encoder exits retain their existing asynchronous teardown behavior. The queue drain waits for its short critical section; the nonblocking capture poll is intentionally not used here because lock contention must not truncate terminal fanout.

Input packets do not carry a geometry generation. If a geometry event is published after the worker has refreshed but before it dispatches a packet, that packet can still use the previous map. A queued DOWN that had not yet been observed by the worker cannot be distinguished from a new DOWN after the map changes. After a target mismatch, the capture-owned snapshot is invalidated immediately; an active session receives that invalidation through its next video-loop refresh, or directly at capture return if the display exits. Neither path is an atomic cutover with packet dispatch, and a slow capture unwind can extend the interval before the direct terminal publication. Snapshot refresh and cancellation are serialized once the worker observes the update.

## Executable checks

The focused executable run builds the WGC monitor/policy suite, fifteen transport tests in `tests/unit/test_input_geometry_transport.cpp`, and the event/queue concurrency tests in `tests/unit/test_thread_safe.cpp`. Coverage includes field-preserving mapping updates, generation ordering and stale rejection, touch/pen cancellation-before-publication and stale-contact suppression, invalid/closed capture behavior, invalid startup mapping until a verified sample, periodic target identity checks during color-event subscription, worker wake coalescing, invalidation on unverified geometry, terminal-generation rejection of delayed publishers, generation-aware input reads, capture-return queue drain before delayed-port rejection, deterministic out-of-order event publication races through the event API and dynamic geometry publisher, and terminal geometry publication without a reinit signal on capture-error exits.

Direct build/run from the candidate:

```sh
g++ -std=c++23 -Wall -Wextra -I . -I /home/chasep/Documents/Codex/2026-10-20/we-x20/work/deps \
  tests/tests_main.cpp tests/unit/test_wgc_monitor_selection.cpp \
  tests/unit/test_input_geometry_transport.cpp tests/unit/test_thread_safe.cpp \
  -lgtest -pthread -o /tmp/issue301-focused-tests/vibepollo-focused-tests
/tmp/issue301-focused-tests/vibepollo-focused-tests
```

The focused executable passed **70/70 tests**. The absolute-mouse script passed **11/11 cases** and the Guide session-config harness passed **7/7**. Earlier runs also passed Guide permission, RTSP Guide handoff, and monitor-profile handoff harnesses at **9/9, 1/1, and 4/4** after supplying inert geometry refresh hooks outside those harnesses' contracts. Syntax-only g++ checks passed for production `src/input.cpp` and `src/video.cpp` in both source and target trees using existing pinned dependency headers. Windows-specific production files were not compiled because Windows SDK/WRL/C++/WinRT headers are unavailable; no WGC hardware or native pointer runtime test was available.

## Limits and next validation

The portable tests prove policy, synchronized snapshot-store behavior, event ordering and contact-transport decisions, not Windows pointer injection, repeat-callback execution, physical-display mapping, the publication-to-dispatch race, or client behavior. The C++/WinRT/DXGI callsite and encoder/input integration still need a Windows build. Native qualification must then verify ongoing WGC frames, exact target identity, virtual-desktop origin/extent transitions, absolute mouse normalization, touch and pen repeat cancellation, stale-contact handling, and capture-close/reinit behavior.

Even if those checks pass, this change only establishes the input transport behavior under the tested transitions. It does not show that WGC capture suppression caused or fixes the reporter's freeze; frame arrival, publication, target lifetime, and restart timing require separate issue evidence.
