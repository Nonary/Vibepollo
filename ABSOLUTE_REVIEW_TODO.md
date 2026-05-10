# Review work done, what is necessary to be fixed

## First findings

### 1. Medium, High in elevated Windows deployments: unsafe NVML DLL loading

The Windows host stats provider loads `nvml.dll` using the default DLL search order. If the app runs elevated or as a service, this can create a DLL search order hijack risk and potentially local privilege escalation.

**What should be done:**

Use safe DLL loading APIs and restrict resolution to trusted locations. Prefer `LoadLibraryEx` with safe search flags, trusted NVIDIA install paths, and path or signature validation when loading outside `System32`. Also clean up module handle lifetime, since the current flow can leak the handle if initialization fails.

The Linux `dlopen("libnvidia-ml.so.1", ...)` path is lower risk in normal service deployments, but should be hardened consistently where practical.

**Status (2026-05-10):** **Done.** `src/platform/windows/host_stats.cpp` now loads NVML from trusted locations only and cleans up module ownership on failure/destruction, and `src/platform/linux/host_stats.cpp` now also resolves NVML only from trusted absolute library paths instead of relying on loader search-order lookup.

### 2. Medium: unbounded samples/events can grow the DB and produce oversized API responses

Completed sessions are capped, but samples and events per session are not. A long-running session can keep accumulating rows every 2 seconds. The detail endpoint then loads all samples and events into memory and serializes them in one response.

This creates DB growth risk, including WAL/SHM sidecars, and an authenticated request can trigger a large read, memory allocation, JSON serialization, and response.

**What should be done:**

Add retention at the samples/events layer. Cap rows per session, add TTL or DB size quota, and expose pagination or time-windowing on the detail API. Return summary or downsampled chart data by default rather than full raw history.

**Status (2026-05-10):** **Done.** The detail API now returns capped default drawer payloads, reports `total_samples` / `total_events`, marks truncation, allows explicit `?full=1` fetches for full export, the writer enforces per-session row caps for samples and events, and session-history pruning now also supports configurable TTL-based cleanup plus an approximate live DB-size quota.

### 3. Medium: local telemetry DB persists sensitive metadata without explicit permission hardening

The new `session_history.db` persists client names, device names, app names, codec and stream settings, CPU/GPU model strings, host counters, and network throughput/interface metadata. Because of the sensitive data nature and the fact it logs sensitive details like IP, GPU, etc even at log level of INFO, we should make this database admin restricted.

**What should be done:**

Set restrictive permissions on `session_history.db`, `session_history.db-wal`, and `session_history.db-shm`. On POSIX, use `0600` and ensure the containing state directory is not world-readable if it contains sensitive data. On Windows, restrict access to Administrators & SYSTEM

Also consider a config option to disable persistence or shorten retention.

**Status (2026-05-10):** **Done.** `src/session_history.cpp` now tightens the containing history directory plus `session_history.db`, `-wal`, and `-shm` during init (owner-only directory/file permissions on non-Windows, Administrators+SYSTEM DACL on Windows), and configuration now allows disabling session-history persistence entirely or shortening retention with TTL / DB-quota controls.

### 4. Medium/Low: deleting active sessions can orphan samples/events and corrupt history state

The DELETE endpoint can delete any session UUID, including an active session. The active in-memory session remains and may continue writing samples/events for that UUID.

SQLite foreign keys are declared, but the PR does not appear to enable `PRAGMA foreign_keys = ON`, so orphaned rows can remain. These rows may not show in detail views and may not be cleaned up by pruning logic tied to the parent session table.

**What should be done:**

Reject deletion of active sessions, ideally with `409 Conflict`. Enable SQLite foreign keys on every connection and use `ON DELETE CASCADE` for dependent samples/events. Even with cascade enabled, active-session deletion should still be blocked unless the intended behavior is to terminate and delete the stream.

**Status (2026-05-10):** **Done.** Active-session deletion is rejected with `409 Conflict`, foreign keys are enabled on both read/write connections, and delete outcomes now map to proper HTTP statuses instead of a blanket `404`.

### 5. Low/Medium: client-reported counters are persisted without sufficient validation

The PR persists counters derived from client control packets. A malicious or buggy client can report negative or extremely large values, corrupting history and event verdicts.

The existing direct pointer casts are also fragile for short payloads, alignment, and implicit endianness assumptions.

**What should be done:**

Validate packet sizes before reading, decode with safe copy semantics, handle endianness explicitly, and clamp values to sane bounds. Apply this to loss stats, invalidate-ref-frame payloads, and any other control messages that read directly from `payload.data()`.

**Status (2026-05-10):** **Done.** The RTSP control-payload paths now validate payload size, decode via safe copy semantics, use explicit endianness handling, and clamp persisted loss values before they affect history/verdicts.

---

## Performance Findings

### 1. High: history writer queue is unbounded and accepts work when history is not running

`session_history::enqueue()` always appends to the global write queue and has no capacity limit or disabled-state check. Public entry points such as `begin_session()`, `end_session()`, and `record_event()` enqueue commands without checking whether the history subsystem is fully initialized or whether the writer thread is alive.

If database initialization fails before the writer starts, later stream lifecycle calls can still enqueue commands forever. If the writer is alive but stalled by slow I/O, disk-full behavior, or a locked database, the sampler can continue adding per-session samples every two seconds while active sessions exist. That creates an unbounded memory growth path in addition to any on-disk retention concerns.

**What should be done:**

Make `enqueue()` return failure when history is not running, add a bounded queue / high-water mark, and drop or coalesce low-value samples under pressure. Lifecycle events and deletes should have higher priority than periodic samples. Log dropped samples or expose a counter in host/session stats so data loss is visible.

**Status (2026-05-10):** **Done.** `enqueue()` now fails when history is unavailable, the writer queue has a high-water mark for sample traffic, and low-value samples are dropped under pressure instead of growing memory unboundedly.

### 2. High: SQLite write failures are mostly ignored

Several write paths call `sqlite3_step()` and ignore the return code, including session begin, session end, sample insert, and event insert. The writer also does not fail the batch when `BEGIN TRANSACTION` fails, and most command types do not surface write failure to callers.

This means disk-full errors, locked/corrupt databases, migration mistakes, or statement failures can silently lose history while the UI continues as if recording succeeded. The duplicate-UUID warning after `process_begin()` can also become misleading because it checks `sqlite3_changes()` after an unchecked insert step.

**What should be done:**

Have each write helper return status, require `SQLITE_DONE` where expected, log `sqlite3_errmsg()` on failure, and rollback the transaction when a batch fails. Add a busy timeout or retry policy for transient lock contention. Consider exposing a degraded/unavailable history state so the API and UI do not report silent success.

**Status (2026-05-10):** **Done.** Write helpers now return success/failure, transaction begin/commit failures are handled explicitly, batch failures roll back, SQLite errors are logged, and busy timeouts are configured.

### 3. High: DELETE history can block an HTTP worker indefinitely

The history DELETE HTTP handler calls `session_history::delete_session()` synchronously. `delete_session()` enqueues a writer command and then waits on `future.get()` without a timeout.

If the writer thread is stuck on I/O, processing a large backlog, or unexpectedly stopped after the initial availability check, the HTTP request can block for a long time. The current handler also maps all failures to `404`, which conflates a confirmed missing session with a history subsystem failure or writer timeout.

**What should be done:**

Use `future.wait_for()` with a bounded timeout and return `503` or `504` when the writer is unavailable or the operation times out. Keep `404` only for a confirmed missing session. Consider rejecting deletes while the writer queue is above a high-water mark.

**Status (2026-05-10):** **Done.** History deletion now waits with a bounded timeout and distinguishes `404`, `409`, `503`, and `504` outcomes instead of blocking indefinitely or flattening errors into `404`.

### 4. High: the shared read-side SQLite connection is used without explicit serialization

`g_read_db` is a single global SQLite connection used directly by `list_sessions()` and `get_session_detail()`. Those APIs are called from HTTP handlers and can run concurrently.

This may be memory-safe only if SQLite is built and opened in serialized mode, but the code does not enforce that with an explicit mutex or `SQLITE_OPEN_FULLMUTEX`. If SQLite is built in multi-thread mode, concurrent use of one `sqlite3*` handle is unsafe. Even in serialized mode, one shared handle means large detail reads can block other history reads. The frontend grouped detail path can amplify this by issuing many detail requests in parallel.

**What should be done:**

Add a read mutex at minimum. A better long-term design is a small read-only connection pool or per-request read connection with busy timeout. Also limit grouped detail request concurrency in the frontend.

**Status (2026-05-10):** **Done.** Grouped history detail fetches are concurrency-limited in the frontend, and the backend no longer shares one long-lived read connection across requests: read APIs now open per-request full-mutex SQLite connections, falling back to the write handle only when a fresh read connection is unavailable.

### 5. High: RTSP active stats use the wrong identity and can duplicate one session while omitting another

`stream::get_all_session_info()` first asks RTSP for all session UUIDs and then calls `rtsp_stream::find_session(uuid)` for each value. The UUID list is built from `stream::session::uuid(*session)`, which returns `device_uuid`, but the returned active-session info uses `session->history_uuid` as `info.uuid`.

If the same client/device has more than one active RTSP session, the UUID list can contain duplicates. `find_session()` then returns the first matching device UUID each time, so stats for one stream can be duplicated while another active stream is omitted. The current design is also O(n²) because each lookup scans the session list again.

**What should be done:**

Expose a snapshot API that copies all needed per-session data while holding the RTSP session-list lock once. Do not look up per-stream stats by `device_uuid`; use `history_uuid` as the per-stream identity throughout the active stats and history paths.

**Status (2026-05-10):** **Done.** RTSP active-session reads now use a one-pass snapshot instead of repeated `device_uuid` lookups, so duplicated/omitted active-session reporting is fixed.

### 6. High: active-session polling leaks after navigation and can overlap under backend slowness

`ActiveSessionsCard` starts the sessions store polling loop on mount, but it does not stop polling on unmount. The store has `stopPolling()`, but there does not appear to be a caller.

The polling loop also uses `setInterval()` and starts a new poll every two seconds even if the previous poll has not completed. Each poll fans out into multiple HTTP requests, so backend slowness can cause overlapping requests and multiply load.

**What should be done:**

Mirror the host stats component pattern by stopping polling in `onBeforeUnmount()`. Add an in-flight guard or `AbortController` so ticks are skipped/cancelled while a prior poll is pending, and add backoff after repeated failures.

**Status (2026-05-10):** **Done.** Active-session polling now stops on unmount, uses a shared in-flight guard/reference-counted lifecycle, and backs off after repeated poll failures instead of hammering the backend at a fixed cadence.

### 7. Medium: missing indexes for history list, prune, and event detail queries

The schema indexes samples by session and timestamp, but completed-session listing and pruning order by `end_time_unix` without a matching index. Event detail reads filter by session and order by timestamp, but only `events(session_uuid)` exists.

The current completed-session retention cap keeps normal steady-state costs bounded, but this becomes unnecessary work after migrations, failed pruning, manual DB growth, or future retention changes.

**What should be done:**

Add an index for completed sessions ordered by end time, for example:

```sql
CREATE INDEX IF NOT EXISTS idx_sessions_end_time
ON sessions(end_time_unix DESC)
WHERE end_time_unix IS NOT NULL;
```

Also consider:

```sql
CREATE INDEX IF NOT EXISTS idx_events_time
ON events(session_uuid, timestamp_unix);
```

**Status (2026-05-10):** **Done.** The missing event-time and completed-session end-time indexes were added.

### 8. Medium: `host_stats::start()` double-start guard can stop the real sampler

If `host_stats::start()` is called while the sampler is already running, it returns a new `deinit_t`. Destroying that second guard stops the global sampler and resets the global provider, even though the first guard is still supposed to own the running sampler.

A harmful sequence is:

```cpp
auto first = host_stats::start();
auto second = host_stats::start();
second.reset(); // stops the sampler that first owns
```

The existing double-start unit test only checks that the second start does not crash; it does not verify that destroying the second guard leaves the first sampler running.

**What should be done:**

Protect start/stop with a mutex and return `nullptr` or a true no-op guard on double start. Another safe option is a generation token so only the guard that actually started the sampler can stop it. Add a regression test that destroys the second guard and verifies the sampler remains owned by the first guard.

**Status (2026-05-10):** **Done.** `src/host_stats.cpp` now uses generation-based ownership so only the real starter can stop the sampler, and `tests/unit/test_host_stats.cpp` now covers the regression where destroying the second `start()` guard must not stop the first owner's sampler.

### 9. Medium: network throughput can spike after interface reset or interface change

Linux handles network counter decrease by resetting the baseline without emitting a spike. macOS does unsigned subtraction without checking whether counters decreased. Windows periodically refreshes the primary interface, but it does not reset the RX/TX baseline when the selected interface changes.

After VPN changes, sleep/wake, Wi-Fi/Ethernet changes, or interface reset, this can emit a huge synthetic throughput value. That value can appear in live charts and can also be persisted in session samples.

**What should be done:**

Track interface identity as part of the network baseline. Reset the baseline when interface identity changes or counters decrease. If link speed is available, clamp impossible deltas to a sane maximum.

**Status (2026-05-10):** **Done.** Windows, macOS, and Linux now reset network baselines on interface changes or counter rollback, and all three platforms clamp sampled RX/TX throughput to sane per-interface maxima when link-speed information is available.

### 10. Medium: frontend history chart annotations are O(events × samples)

`SessionCharts` resolves each event annotation by scanning every displayed data point to find the closest sample. History samples are timestamp ordered, and events from the backend are ordered by timestamp, so this does unnecessary repeated work.

This is low risk for short sessions, but it becomes noticeable with long sessions or grouped history views.

**What should be done:**

Use binary search per event, or a single two-pointer scan after sorting events and samples by timestamp.

**Status (2026-05-10):** **Done.** History chart event annotations now resolve event timestamps to samples with a binary search instead of scanning every displayed sample for every event.

### 11. Medium: RTSP session metadata can be read without a consistent lock during stats snapshots

The active stats path reads mutable RTSP session fields such as `device_name` and configuration metadata while other code can mutate some session metadata. Atomic counters are fine, but string/config metadata should be copied under a consistent lock or made immutable for the lifetime of the session.

This becomes more important because the stats path now runs from both the sampler thread and HTTP handlers.

**What should be done:**

Use the snapshot API suggested above for RTSP stats so all mutable metadata is copied while holding the session-list/session-metadata lock once. Alternatively, add explicit locking around mutable session metadata reads/writes.

**Status (2026-05-10):** **Done.** Active RTSP stats now copy mutable metadata under explicit locking/snapshot boundaries instead of reading those fields unsafely.

---

## Bug Findings

### 1. High: Early returns after session history startup can terminate the process

The session history subsystem starts background writer and sampler threads during startup. Normal shutdown joins those threads, but there are startup failure and early-shutdown paths after session history initialization that return before calling the session history shutdown path.

That can leave joinable static `std::thread` objects alive during process teardown. Destroying a joinable `std::thread` calls `std::terminate()`, so an otherwise ordinary startup failure or early shutdown can become a hard process abort.

**What should be done:**

Wrap session history initialization in an RAII shutdown guard, or ensure every return path after successful session history startup calls the matching shutdown function before returning.

The shutdown path should also be safe if initialization only partially completed.

**Status (2026-05-10):** **Done.** `main.cpp` now ensures `session_history::shutdown()` still runs on early returns after history startup, avoiding joinable-thread termination during teardown.

---

### 2. Medium: Session end and sampler can race, producing samples after the session is ended

The sampler checks for active sessions and then gathers RTSP/WebRTC snapshots before enqueueing sample writes. At the same time, session end removes the session from active state and queues the final end operation.

A sampler tick can pass the active check just before the session ends, then enqueue a sample after the end operation has already been queued. In that interleaving, the writer can compute the session verdict before the final sample exists, and then insert a sample after the session has already been marked ended.

That can make history details and final verdicts inconsistent, especially for drops, stalls, bitrate changes, or latency spikes that happen right at the end of a stream.

**What should be done:**

Make session lifecycle ordering explicit in the writer. Late samples or events for ended sessions should either be discarded or applied before the end verdict is finalized.

Add a regression test for a sampler/end interleaving where a sample is queued immediately after the session end command.

**Status (2026-05-10):** **Done.** Late samples/events for ended or missing sessions are now rejected so history rows cannot be appended after end finalization, and `tests/unit/test_session_history.cpp` now covers the post-`end_session()` late-event and late-sample interleavings explicitly.

---

### 3. Low: Session detail API response does not match the frontend sample/event contract

The frontend session-history types require each sample and event to include its `session_uuid`, but the session detail API serializes samples and events without that field.

The UI reconstructs the UUID in some grouped-history paths, but the raw API response still does not match the declared frontend shape. This makes the contract easy to misuse and means exported or raw detail data is not self-contained.

**What should be done:**

Either include `session_uuid` in each serialized sample and event, or make the frontend fields optional and handle both shapes explicitly.

Including the field in the API is the cleaner option because it keeps raw history data self-contained.

**Status (2026-05-10):** **Done.** The detail API now includes `session_uuid` in serialized samples and events, so the raw payload matches the frontend contract, and the unit suite now locks in both the per-row UUID contract and the default truncation/`include_all` behavior.

---

## Maintainability Findings

### 1. Medium: `session_history.cpp` is doing too much

`src/session_history.cpp` contains schema setup, migrations, SQLite statement binding, writer queueing, sampler logic, live aggregation, event detection, read APIs, deletion, and lifecycle management in one large translation unit.

That makes the subsystem hard to review safely because unrelated concerns share the same file and private state. Future changes to sampling, migrations, retention, active sessions, or read APIs all have to touch the same dense implementation.

The writer loop is also repetitive. The normal batch path and final drain path both dispatch mostly the same command switch, which increases the chance that future behavior diverges between normal operation and shutdown.

**What should be done:**

Split the implementation into smaller responsibilities. A reasonable shape would be a SQLite repository for schema, migrations, inserts, queries, and deletes; a writer component for queueing, transactions, and completions; a sampler component for RTSP/WebRTC polling, aggregators, and event detection; and a small service/facade for public lifecycle and API functions.

At minimum, extract the repeated writer command dispatch into one helper used by both the normal loop and final drain path.

**Status (2026-05-10):** **Done.** The storage/schema/migration/query layer lives in `src/session_history_storage.cpp` / `src/session_history_storage.h`, the writer queue and transaction/readback flow now live in `src/session_history_writer.cpp` / `src/session_history_writer.h`, the active-session polling and aggregation flow now live in `src/session_history_sampler.cpp` / `src/session_history_sampler.h`, and `src/session_history.cpp` is reduced to the public facade/lifecycle wiring.

### 2. Medium: JSON response serialization is duplicated across handlers

`src/confighttp.cpp` manually builds session-related JSON in several separate handlers: live RTSP sessions, history list, history detail, and active history sessions. Many fields are mapped repeatedly by hand.

This makes the API shape easy to drift over time. Adding, renaming, rounding, or changing one field requires finding every manual serializer and keeping all variants consistent.

**What should be done:**

Centralize response serialization behind small helpers, for example:

```cpp
nlohmann::json to_json(const session_history::session_summary_t &s);
nlohmann::json to_json(const session_history::session_sample_t &s);
nlohmann::json to_json(const session_history::session_event_t &e);
nlohmann::json to_json(const session_history::active_session_t &s);
nlohmann::json to_json(const platf::host_stats_t &s);
```

Handlers should then compose those helpers instead of repeating field-by-field mapping.

**Status (2026-05-10):** **Done.** `src/confighttp.cpp` now uses shared serializer helpers for session summaries, samples, events, active sessions, and full detail payloads instead of repeating the field mapping inline across handlers.

### 3. Medium: RTSP and WebRTC sampling logic is mostly copy-paste

`sample_rtsp_sessions()` and `sample_webrtc_sessions()` repeat the same broad flow: compute current totals, update an aggregator, detect events, populate a `session_sample_t`, copy host stats into the sample, and enqueue the insert.

The host snapshot population is duplicated as well, including RAM and VRAM percentage calculations. Any future change to host sample semantics has to be made in multiple places.

**What should be done:**

Extract common helpers for shared sampling work, such as:

```cpp
void populate_host_snapshot(session_sample_t &sample, const platf::host_stats_t &host);
void enqueue_sample(session_sample_t sample);
sample_metrics_t update_aggregator(...);
```

Then each protocol-specific sampler only needs to translate protocol-specific counters into the common sample model.

**Status (2026-05-10):** **Done.** RTSP and WebRTC sampling now share one sampled-session path for aggregator updates, event detection, host snapshot population, and enqueueing; the protocol-specific pieces are reduced to producing the raw per-session counters that feed that shared path.

### 4. Medium: base schema and migrations are not in sync

The base schema in `SCHEMA_SQL` does not represent the complete current fresh-install schema. Some columns are present in the base schema, while other current columns are added later through phase-style migrations.

This works because migrations run after schema creation, but it makes the schema harder to reason about. A fresh database should be understandable from the base schema alone, while migrations should explain upgrades from older versions.

**What should be done:**

Update the base schema so it reflects the current fresh-install table shape. Keep migrations only for upgrading existing databases.

Also consider using `PRAGMA user_version` and explicit versioned migration steps rather than phase comments. That would make future schema changes easier to audit and test.

**Status (2026-05-10):** **Done.** The fresh-install base schema now matches the current install shape for this feature set, and `session_history` now tracks upgrades with `PRAGMA user_version` plus explicit versioned migration steps guarded for older databases.

### 5. Low/Medium: bitrate naming is inconsistent across backend, history, and UI models

The PR uses several names for closely related bitrate concepts, including `client_requested_bitrate`, `client_bitrate_kbps`, `target_requested_bitrate_kbps`, and `target_bitrate_kbps`.

The comments explain the difference, but the naming is hard to follow when moving between stream configuration, live stats, persisted history, and frontend types.

**What should be done:**

Pick two canonical terms and use them consistently everywhere. For example:

```text
client_requested_bitrate_kbps
effective_encoder_bitrate_kbps
```

or:

```text
requested_wire_bitrate_kbps
encoder_bitrate_kbps
```

The important part is that the same concept has the same name in C++ structs, JSON fields, TypeScript types, and UI labels.

**Status (2026-05-10):** **Done.** The live/session-history/API/frontend models and the SQLite schema now consistently use `requested_bitrate_kbps` and `encoder_bitrate_kbps`, and the migration path renames older session-history databases away from the pre-release `target_*` column names.

### 6. Low/Medium: codec-name mapping is duplicated and inconsistent

Codec names are mapped in multiple places. Some code maps unknown values to `"Unknown"`, while another path maps any value other than H.264 or HEVC to AV1.

That makes the UI and persisted history depend on which path produced the string, and future codec additions will require updating several call sites.

**What should be done:**

Move codec formatting into one shared helper, for example:

```cpp
std::string_view codec_name(int video_format) {
  switch (video_format) {
    case 0: return "H.264";
    case 1: return "HEVC";
    case 2: return "AV1";
    default: return "Unknown";
  }
}
```

Use that helper everywhere live session metadata, history metadata, and HTTP responses need a codec label.

**Status (2026-05-10):** **Done.** Codec display-name normalization now goes through shared `stream` helpers for RTSP and WebRTC paths, and the canonical labels are reused for live-session responses, active-session/history metadata, and persisted session-history summaries.

### 7. Low: `total_losses` and `total_frames` are misleading variable names

In the session-end verdict calculation, the SQL query uses `MAX(client_reported_losses)` and `MAX(frames_sent)`, but the local variables are named `total_losses` and `total_frames`.

Those values are cumulative counter maxima, not sums. The current names make the verdict calculation harder to read and can mislead future maintainers into thinking the query returns totals.

**What should be done:**

Rename the locals to match what they actually contain, for example:

```cpp
max_reported_losses
max_frames_sent
```

**Status (2026-05-10):** **Done.** The misleading verdict locals were renamed to reflect that they hold counter maxima, not sums.

### 8. Low: event detection hardcodes the sampling interval in user-visible text

Stall detection reports duration using `agg.zero_frame_ticks * 2`. That assumes the sampler interval is always two seconds.

If `SAMPLE_INTERVAL` changes later, the event text will be wrong even though the detection logic still compiles and runs.

**What should be done:**

Use the existing interval constant when formatting the stall duration, for example:

```cpp
auto stall_seconds =
  agg.zero_frame_ticks *
  std::chrono::duration_cast<std::chrono::seconds>(SAMPLE_INTERVAL).count();
```

**Status (2026-05-10):** **Done.** Stall event text now derives its seconds value from `SAMPLE_INTERVAL` instead of hardcoding `2`.

### 9. Low/Medium: Windows GPU selection is a weak abstraction

The Windows host stats provider picks the non-software DXGI adapter with the largest dedicated VRAM. That may not be the GPU actually used for capture or encoding on hybrid, eGPU, or multi-GPU systems.

If the UI presents these stats as stream-related host context, the selected GPU can be misleading on machines where capture or encode runs on a different adapter.

**What should be done:**

Make the limitation explicit in code comments and UI wording, or connect host GPU stats to the adapter selected by the capture/encoder path.

Longer term, the platform abstraction should support selecting or reporting the actual stream adapter rather than independently guessing based on VRAM size.

**Status (2026-05-10):** **Done.** Host GPU wording still clearly describes the best-effort host-side adapter selection, and session/live/history paths now also persist and expose a separate `stream_gpu_model` field sourced from the active Windows DXGI/WGC capture adapter when a reliable adapter LUID is available.

### 10. Medium: `ActiveSessionsCard.vue` and `SessionCharts.vue` are large and repetitive

`ActiveSessionsCard.vue` mixes session presentation, formatting, chart toggles, protocol branching, and polling ownership. `SessionCharts.vue` is much larger and contains formatting, live/history branching, chart dataset construction, annotation handling, host chart construction, and modal/zoom behavior.

The size makes these components difficult to review and risky to extend. Small UI changes can require editing a very large component with several intertwined responsibilities.

**What should be done:**

Extract composables and smaller components around stable responsibilities, for example:

```text
useSessionFormatting()
useLiveSessionChartData()
useHistorySessionChartData()
SessionStatsGrid.vue
SessionChartPanel.vue
```

For charts, consider a small dataset/config factory so adding a metric does not require duplicating chart construction logic across live and history modes.

**Status (2026-05-10):** **Done.** `ActiveSessionsCard.vue` now delegates protocol-specific rendering to `components/session/ActiveRtspSessionsSection.vue` and `components/session/ActiveWebRtcSessionsSection.vue`, shared formatting moved into `components/session/sessionFormatting.ts`, and `SessionCharts.vue` now uses reusable `SessionChartPanel.vue` / `SessionChartZoomModal.vue` wrappers for the repeated chart shell and modal UI.

### 11. Low: frontend session services are inconsistent and partly unused

`fetchActiveSessions()` exists in the session API service, but the active session flow does not appear to use it. The code also mixes absolute `/api/...` paths in services with relative `./api/...` paths in the sessions store.

This creates two competing API access patterns for the same feature area and makes future refactors more error-prone.

**What should be done:**

Pick one convention for frontend API calls and apply it consistently. Prefer routing all session API calls through the service layer, then have stores/components call that service instead of constructing endpoint paths directly.

Remove unused service functions if they are not part of the intended API surface.

**Status (2026-05-10):** **Done.** The active-session polling store now goes through the shared session service helpers instead of constructing its own `./api/...` calls, so the frontend session API flow uses one consistent service-layer pattern.

