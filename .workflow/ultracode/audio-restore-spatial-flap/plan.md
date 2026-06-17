# Audio Restore Spatial Flap

## Goal
Fix the reported Vibeshine behavior where ending a stream does not restore the correct audio output, and first stream startup causes Windows Sound Settings spatial selection to churn rapidly with high service CPU.

## Success criteria
- Identify the code path that changes the default audio render device for a stream.
- Restore the pre-session default endpoint reliably when the stream ends.
- Avoid repeated or unstable default-device writes during first stream startup.
- Keep the change scoped to audio control behavior.
- Verify with the smallest relevant build or tests.

## Current context
- User has no logs from the affected system.
- `src/audio.cpp` owns audio-context lifetime and calls platform audio control.
- The suspected platform implementation is Windows audio endpoint control.

## Constraints
- Work from `D:\sources\sunshine`.
- Use PowerShell-compatible commands.
- Do not run full test suite by default.
- Do not package installer or symbols unless producing an installable build.
- Do not commit or push.

## Risk level
Medium. Audio default-device changes are user-visible and Windows-specific, but the intended blast radius is the audio restore module.

## Approval gates
No destructive or publishing actions are planned. No approval needed for local source edits and targeted build.

## Mode
Delegated. Native read-only agents are available and useful for independent code-path discovery.

## Work packets
- `01-audio-restore-flow`: trace session default-device capture/restore flow.
- `02-spatial-flap-hypothesis`: inspect Windows audio control for repeated endpoint writes or spatial side effects.
- `03-implementation`: parent-owned code change after integrating packet evidence.

## Eval contract
- Outcome: stream startup changes default audio only when needed, and stream teardown restores the captured prior default.
- Shared surfaces: `audio_ctx_t`, `src/audio.cpp`, Windows `audio_control_t` implementation.
- Required checks: targeted compile/build of affected target or full native build if target granularity is unavailable.
- Blocking conditions: inability to find Windows audio control implementation, build tree unavailable, or API uncertainty that would risk destructive endpoint behavior.
- Handoff evidence: file paths and symbols for restore flow, repeated write sites, and verification command output.

## Integration policy
The parent session owns edits. Read-only agents provide evidence only. Conflicts are resolved by source inspection and the narrowest behavior-preserving change.

## Verification plan
- Inspect final diff.
- Run a targeted native build using `cmake --build d:/sources/sunshine/build --config Debug --target all -j 10` with MSYS2 path prepended unless a narrower reliable target is found.
- Skip packaging because this is compile-only verification.

## Completion criteria
- Source fix is applied.
- Workflow result notes and final report are updated.
- Verification status is recorded honestly.
