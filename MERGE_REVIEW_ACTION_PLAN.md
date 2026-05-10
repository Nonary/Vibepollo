# Merge Readiness Master Plan

Source of truth reviewed: `ABSOLUTE_REVIEW_TODO.md`

This plan is intentionally **complete**: every finding in the review file is
placed into a work phase and given a disposition:

- **must before merge**
- **should before merge**
- **defer after merge**

The goal is to avoid missing anything while still sequencing the work sanely.

## Planning principles

1. **Correctness and safety first.** Anything that can corrupt state, deadlock,
   terminate the process, silently lose data, or create a security issue comes
   before cleanup/refactor work.
2. **Phase by subsystem.** `session_history`, active stats, security hardening,
   and frontend polling should not be mixed into one giant commit.
3. **Every review item gets an explicit home.** Nothing in
   `ABSOLUTE_REVIEW_TODO.md` is left as an implied “maybe later”.
4. **Refactors are last unless required.** Large maintainability work is real,
   but it should not destabilize the blocker fixes.

## Work timeline

## Phase 0 - Reconfirm all findings at current HEAD

Goal: verify the review against the current branch state before changing code.

Why this phase exists:

- some issues may already be fixed by follow-up commits,
- some may partially overlap,
- some may be best solved together instead of one-by-one.

Files to inspect first:

- `src/session_history.cpp`
- `src/session_history.h`
- `src/confighttp.cpp`
- `src/main.cpp`
- `src/stream.cpp`
- `src/stream.h`
- `src/platform/windows/host_stats.cpp`
- `src/platform/linux/host_stats.cpp`
- `src/platform/macos/host_stats.mm`
- `src/host_stats.cpp`
- `src_assets/common/assets/web/components/ActiveSessionsCard.vue`
- `src_assets/common/assets/web/components/SessionCharts.vue`
- `src_assets/common/assets/web/components/SessionHistory*.vue`
- `src_assets/common/assets/web/stores/sessions.ts`
- `src_assets/common/assets/web/types/*.ts`
- `tests/unit/test_session_history.cpp`
- `tests/unit/test_host_stats.cpp`

Deliverable:

- one triage sheet marking each finding as `confirmed`, `already fixed`, or
  `defer`
- exact implementation order for the confirmed items

## Phase 1 - Session history safety and lifecycle

Disposition: **must before merge**

This phase handles the highest-risk `session_history` issues.

Review items covered:

1. **Performance 1** - unbounded writer queue / accepts work when history is not running
2. **Performance 2** - SQLite write failures mostly ignored
3. **Performance 3** - DELETE can block an HTTP worker indefinitely
4. **Performance 4** - shared read-side SQLite connection lacks explicit serialization
5. **Bug 1** - early returns after history startup can terminate the process
6. **Bug 2** - sampler/end race can write samples after session end
7. **First findings 4** - deleting active sessions can orphan rows / corrupt history state

Primary files:

- `src/session_history.cpp`
- `src/session_history.h`
- `src/confighttp.cpp`
- `src/main.cpp`
- `tests/unit/test_session_history.cpp`

Planned work:

1. make history startup/shutdown RAII-safe so every post-start return path
   performs safe teardown;
2. make queue admission explicit:
   - reject or fail fast when history is not running,
   - introduce a high-water mark,
   - drop/coalesce low-priority samples under pressure,
   - keep lifecycle commands and deletes higher priority than periodic samples;
3. require every write helper to return success/failure and log SQLite errors;
4. fail and rollback the batch when transaction or statement execution fails;
5. add bounded waiting for delete completion and return proper HTTP status codes;
6. reject deletion of active sessions, ideally `409 Conflict`;
7. enable and rely on foreign keys on every connection;
8. serialize all read-side DB access explicitly;
9. define ordering for late samples/events after session end:
   - either reject them for ended sessions,
   - or ensure they are applied before final verdict/end finalization;
10. add regression tests for:
   - double return/shutdown safety,
   - delete timeout/unavailable path,
   - active-session delete rejection,
   - late-sample after end interleaving.

## Phase 2 - Active RTSP session correctness and frontend polling

Disposition: **must before merge**

Review items covered:

1. **Performance 5** - RTSP active stats use the wrong identity and can duplicate/omit sessions
2. **Performance 6** - active-session polling leaks after navigation and overlaps under slowness
3. **Performance 11** - RTSP session metadata read without a consistent lock

Primary files:

- `src/stream.cpp`
- `src/stream.h`
- `src/confighttp.cpp`
- `src_assets/common/assets/web/components/ActiveSessionsCard.vue`
- `src_assets/common/assets/web/stores/sessions.ts`

Planned work:

1. replace repeated per-session lookup with a one-pass locked snapshot API;
2. keep per-stream identity consistent all the way through active stats;
3. copy mutable metadata under one consistent lock/snapshot boundary;
4. stop active polling on component unmount;
5. prevent overlapping poll cycles when the previous request is still in flight;
6. add retry/backoff or skip logic after repeated failures;
7. validate that grouped detail or other consumers do not amplify request fan-out unnecessarily.

## Phase 3 - Security and privacy hardening

Disposition: **must before merge** for the first and third item; **should before merge** for DB permissions

Review items covered:

1. **First findings 1** - unsafe NVML DLL loading
2. **First findings 3** - telemetry DB permission hardening
3. **First findings 5** - insufficient validation of client-reported counters

Primary files:

- `src/platform/windows/host_stats.cpp`
- `src/platform/linux/host_stats.cpp` if Linux NVML loading is also hardened now
- packet/control decode paths in `src/stream.cpp` and related handlers
- `src/session_history.cpp`
- platform-specific file permission helpers, if needed
- `tests/unit/test_host_stats.cpp`
- `tests/unit/test_session_history.cpp`

Planned work:

1. move Windows NVML loading to a trusted-path / safe-search strategy;
2. make DLL/module ownership failure-safe so initialization errors do not leak handles;
3. validate payload sizes before decoding client-provided counters;
4. decode with safe copy semantics and explicit endianness handling;
5. clamp persisted values to sane bounds before they affect history or verdicts;
6. restrict permissions on `session_history.db`, `-wal`, and `-shm`;
7. if practical in this phase, make the containing state directory equally restrictive.

## Phase 4 - Operational hardening worth landing before merge

Disposition: **should before merge**

Review items covered:

1. **Performance 7** - missing indexes for history list, prune, and event detail
2. **Performance 8** - `host_stats::start()` double-start guard can stop the real sampler
3. **Performance 9** - network throughput spikes after interface reset/change
4. **Bug 3** - session detail API response does not match frontend sample/event contract

Primary files:

- `src/session_history.cpp`
- `src/host_stats.cpp`
- `src/platform/windows/host_stats.cpp`
- `src/platform/linux/host_stats.cpp`
- `src/platform/macos/host_stats.mm`
- `src_assets/common/assets/web/types/*.ts`
- `src_assets/common/assets/web/components/SessionHistory*.vue`
- `tests/unit/test_host_stats.cpp`
- `tests/unit/test_session_history.cpp`

Planned work:

1. add the missing read/query indexes;
2. fix host-stats double-start ownership so only the real owner can stop the sampler;
3. reset network baselines on interface identity changes or counter decreases;
4. include `session_uuid` in detail samples/events or explicitly relax the frontend contract;
5. add regression coverage where practical.

## Phase 5 - Scalability and retention decisions

Disposition: **should before merge if scope remains safe**, otherwise **defer with an explicit follow-up**

Review items covered:

1. **First findings 2** - unbounded samples/events can grow DB and inflate API responses

Primary files:

- `src/session_history.cpp`
- `src/session_history.h`
- `src/confighttp.cpp`
- `src_assets/common/assets/web/components/SessionHistory*.vue`
- maybe config-related files if retention becomes configurable

Planned work:

1. choose the smallest safe first step:
   - row cap per session,
   - time-window paging,
   - detail API downsampling,
   - or a combination;
2. do **not** redesign the entire history product in the same pass as blocker fixes;
3. if this cannot be landed safely before merge, document it as an explicit post-merge follow-up with limits noted in the PR.

## Phase 6 - Performance/UX cleanup that is useful but not merge-gating

Disposition: **defer after merge unless touched naturally**

Review items covered:

1. **Performance 10** - history chart annotations are O(events x samples)

Primary files:

- `src_assets/common/assets/web/components/SessionCharts.vue`

Planned work:

- replace repeated scans with binary search or a two-pointer walk once the
  correctness and API contracts are stable.

## Phase 7 - Maintainability cleanup backlog

Disposition: **defer after merge unless a blocker fix naturally extracts part of it**

Review items covered:

1. **Maintainability 1** - `session_history.cpp` is doing too much
2. **Maintainability 2** - JSON serialization duplicated across handlers
3. **Maintainability 3** - RTSP/WebRTC sampling logic is mostly copy-paste
4. **Maintainability 4** - base schema and migrations are not in sync
5. **Maintainability 5** - bitrate naming inconsistent
6. **Maintainability 6** - codec-name mapping duplicated/inconsistent
7. **Maintainability 7** - misleading variable names for maxima
8. **Maintainability 8** - stall text hardcodes the sampling interval
9. **Maintainability 9** - Windows GPU selection is a weak abstraction
10. **Maintainability 10** - `ActiveSessionsCard.vue` and `SessionCharts.vue` are too large/repetitive
11. **Maintainability 11** - frontend session services inconsistent / partly unused

Primary files:

- `src/session_history.cpp`
- `src/confighttp.cpp`
- `src/platform/windows/host_stats.cpp`
- `src_assets/common/assets/web/components/ActiveSessionsCard.vue`
- `src_assets/common/assets/web/components/SessionCharts.vue`
- `src_assets/common/assets/web/services/*.ts`
- `src_assets/common/assets/web/stores/*.ts`

Planned work:

1. only extract helpers that directly reduce risk in the blocker fixes;
2. leave major decomposition (`session_history.cpp`, frontend chart split, schema reshaping)
   for a dedicated cleanup pass after merge readiness is achieved;
3. low-risk naming/helper cleanup can be piggybacked only if the surrounding code
   is already being changed for a blocker.

## Phase 8 - Final validation and review ledger update

Disposition: **must before merge**

Validation after each code phase, then once again at the end:

- native build: `ninja sunshine`
- targeted native tests around `SessionHistory*` and `HostStats*`
- frontend build: `npx vite build`
- manual API checks where payload shape or delete behavior changed
- manual UI checks for active polling, session-history detail, and delete behavior

Final output updates:

- update the working review ledger with:
  - fixed
  - intentionally deferred
  - already fixed / dismissed
- update `PR.md` only after the implementation is stable

## Full coverage matrix

Every finding from `ABSOLUTE_REVIEW_TODO.md` mapped to this plan:

| Review section | Finding | Phase | Disposition |
| --- | --- | --- | --- |
| First findings 1 | Unsafe NVML DLL loading | Phase 3 | Must before merge |
| First findings 2 | Unbounded samples/events / oversized detail responses | Phase 5 | Should before merge if safe, else explicit follow-up |
| First findings 3 | DB permission hardening | Phase 3 | Should before merge |
| First findings 4 | Deleting active sessions / foreign keys | Phase 1 | Must before merge |
| First findings 5 | Client-reported counter validation | Phase 3 | Must before merge |
| Performance 1 | Unbounded writer queue / accepts work while disabled | Phase 1 | Must before merge |
| Performance 2 | SQLite write failures ignored | Phase 1 | Must before merge |
| Performance 3 | DELETE can block forever | Phase 1 | Must before merge |
| Performance 4 | Shared read DB lacks serialization | Phase 1 | Must before merge |
| Performance 5 | RTSP active stats wrong identity / duplication | Phase 2 | Must before merge |
| Performance 6 | Active polling leak / overlap | Phase 2 | Must before merge |
| Performance 7 | Missing indexes | Phase 4 | Should before merge |
| Performance 8 | `host_stats::start()` ownership bug | Phase 4 | Should before merge |
| Performance 9 | Network spikes after interface reset/change | Phase 4 | Should before merge |
| Performance 10 | Chart annotation complexity | Phase 6 | Defer after merge |
| Performance 11 | RTSP metadata snapshot locking | Phase 2 | Must before merge |
| Bug 1 | Early returns after startup can terminate process | Phase 1 | Must before merge |
| Bug 2 | Sampler/end race after session end | Phase 1 | Must before merge |
| Bug 3 | Detail API contract mismatch | Phase 4 | Should before merge |
| Maintainability 1 | `session_history.cpp` too large | Phase 7 | Defer after merge |
| Maintainability 2 | JSON serialization duplication | Phase 7 | Defer after merge |
| Maintainability 3 | RTSP/WebRTC sampler duplication | Phase 7 | Defer after merge |
| Maintainability 4 | Base schema and migrations out of sync | Phase 7 | Defer after merge |
| Maintainability 5 | Bitrate naming inconsistent | Phase 7 | Defer after merge |
| Maintainability 6 | Codec-name mapping inconsistent | Phase 7 | Defer after merge |
| Maintainability 7 | Misleading variable names | Phase 7 | Defer after merge |
| Maintainability 8 | Stall text hardcodes interval | Phase 7 | Defer after merge |
| Maintainability 9 | Windows GPU selection abstraction weak | Phase 7 | Defer after merge |
| Maintainability 10 | Large frontend components | Phase 7 | Defer after merge |
| Maintainability 11 | Session services inconsistent / unused | Phase 7 | Defer after merge |

## Recommended implementation order

If we start executing this plan, the recommended order is:

1. Phase 0 - reconfirm
2. Phase 1 - history lifecycle / queue / write correctness
3. Phase 2 - RTSP snapshot + frontend polling
4. Phase 3 - NVML loading + payload validation + DB permissions
5. Phase 4 - indexes + host_stats ownership + network baseline + detail contract
6. Phase 5 - retention/response limits, only if safe to land now
7. Phase 8 - final validation and ledger update
8. Phase 6 and Phase 7 only if explicitly chosen as cleanup follow-up work
