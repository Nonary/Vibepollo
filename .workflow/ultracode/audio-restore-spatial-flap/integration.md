# Integration

## Accepted
- Agent evidence that `set_sink()` could be called from the default endpoint notification callback and rewrite the same endpoint repeatedly.
- Agent evidence that restore retry was skipped when the current default was no longer Steam Streaming Speakers, even if the recorded original endpoint was temporarily absent.

## Rejected
- Broad session teardown reordering. It has wider display/audio lifecycle risk and the narrower audio-control fix is sufficient for the evidence available without logs.
- Temporary trace logging. Useful for follow-up reproduction, but not necessary for a targeted fix.

## Conflicts
None.

## Decisions
- Make `set_sink()` idempotent before calling `SetDeviceFormat()`.
- Skip `SetDefaultEndpoint()` for roles that already point at the requested endpoint.
- Keep a preferred restore pending when the original endpoint is missing and a non-Steam device is currently default.

## Final changes
`src/platform/windows/audio.cpp` now resolves the requested sink endpoint separately from format changes, checks default endpoint state per role, and schedules preferred restore retry for temporarily missing original endpoints.

## Verification still needed
Manual reproduction on a machine with Vibeshine installed and the affected audio endpoint behavior.

## Remaining risks
No user logs were available, and Windows spatial UI behavior is not covered by automated tests.
