# Result 02-spatial-flap-hypothesis: Spatial Flap Hypothesis

## Summary
The likely first-stream CPU/spatial-selection churn was a self-triggering endpoint loop. `set_sink()` wrote `SetDefaultEndpoint()` for all roles even when the requested endpoint was already default, and the WASAPI default-device notification callback could call `set_sink(assigned_sink)` again.

## Evidence
- `src/platform/windows/audio.cpp:890` through `src/platform/windows/audio.cpp:894` installs a callback for virtual sinks that resets the sink after a default endpoint change.
- `src/platform/windows/audio.cpp:969` through `src/platform/windows/audio.cpp:996` previously called `set_format()` and `SetDefaultEndpoint()` without a no-op guard.
- `src/platform/windows/audio.cpp:484` through `src/platform/windows/audio.cpp:518` and `src/platform/windows/audio.cpp:689` through `src/platform/windows/audio.cpp:696` consume default endpoint notifications and trigger audio reinit.
- `src/platform/windows/audio.cpp:138` notes some virtual speaker formats can affect Dolby/DTS spatial audio mode.

## Handoff
Handoff:
- Summary: suppress no-op format/default endpoint writes and skip per-role endpoint writes that already match.
- Changed surfaces: `audio_control_t::set_sink()`.
- Contracts satisfied: startup sink selection remains functional; redundant writes are avoided.
- Assumptions: Windows Sound Settings spatial UI churn is caused by repeated format/default endpoint writes, not an explicit spatial API call.
- Local checks: Debug native build passed.
- Integration evidence: `resolve_sink_device_id()`, role-aware default checks, and no-op guard added.
- Risks: exact spatial UI behavior still needs reproduction on an affected host.

## Files changed
None by agent. Parent integrated the change.

## Decisions
Do not add trace logging yet; make the endpoint writes idempotent first.

## Risks
No automated COM endpoint-policy test exists in this tree.

## Verification run
Inspection only by agent.

## Open questions
Whether affected logs would show repeated `Resetting sink...`, `Changed virtual audio sink format`, and `Reinitializing audio capture`.
