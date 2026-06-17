# Result 01-audio-restore-flow: Audio Restore Flow

## Summary
The pre-session default endpoint is captured in `audio::start_audio_control()` through `audio_control_t::sink_info()`. The first active stream can switch to the selected stream sink in `audio::capture()`, and the last audio-context release calls `audio::stop_audio_control()` to restore the captured host sink and invoke `reset_default_device()`.

## Evidence
- `src/audio.cpp:147` obtains the shared audio context.
- `src/audio.cpp:191` through `src/audio.cpp:195` sets `restore_sink` and switches the sink for the first session.
- `src/audio.cpp:327` through `src/audio.cpp:344` performs teardown restore.
- `src/platform/windows/audio.cpp:791` through `src/platform/windows/audio.cpp:818` captures the host default endpoint and caches preferred restore match fields.
- `src/platform/windows/audio.cpp:1410` and later owns preferred restore retry behavior.

## Handoff
Handoff:
- Summary: restore can fail if the preferred endpoint is temporarily missing while the current default has already moved away from Steam Streaming Speakers.
- Changed surfaces: Windows audio control restore logic.
- Contracts satisfied: preserve user-selected non-Steam default unless the recorded preferred endpoint is missing and needs background retry.
- Assumptions: no user logs; HDMI/DP endpoint re-enumeration is plausible.
- Local checks: Debug native build passed.
- Integration evidence: implemented in `src/platform/windows/audio.cpp`.
- Risks: requires user reproduction to prove Windows endpoint timing.

## Files changed
None by agent. Parent integrated the change.

## Decisions
Keep the change in platform Windows audio control, not session orchestration.

## Risks
Runtime endpoint timing remains environment-dependent.

## Verification run
Inspection only by agent.

## Open questions
Whether the affected user's original endpoint disappears/reappears during display teardown.
