# Result 02-touch-coordinate-mapping: Touch Coordinate Mapping

## Summary
Windows touch/pen normalization subtracted the monitor offset before platform injection added the same offset back into pixel coordinates. For a virtual display to the right, that mapped touch back onto the physical monitor.

## Evidence
- `client_to_touchport()` keeps Windows coordinates monitor-local.
- `monitor_touch_port()` previously subtracted `touch_port.offset_x/y` for every platform.
- `platform/windows/input.cpp` later adds `touchPort.offset_x/y` in `populate_common_pointer_info()`.

## Handoff
Handoff:
- Summary: Normalize touch/pen coordinates as monitor-local on Windows and desktop-relative on Linux.
- Changed surfaces: `src/input.cpp`, `src/input.h`, `tests/unit/test_input.cpp`.
- Contracts satisfied: Right-side virtual display offset no longer shifts normalized touch X negative on Windows.
- Assumptions: Linux inputtino still expects desktop-relative coordinates.
- Local checks: Targeted GTest filter passed.
- Integration evidence: Added `InputTouchMapping.NormalizesUsingPlatformOffsetContract`.
- Risks: Absolute mouse may have a related offset contract issue, but this patch addresses the reported touch/pen path.

## Files changed
- `src/input.cpp`
- `src/input.h`
- `tests/unit/test_input.cpp`

## Decisions
Keep the existing Linux behavior and switch non-Linux touch/pen normalization to monitor-local coordinates.

## Risks
macOS has no touch/pen implementation here, so the non-Linux branch primarily affects Windows.

## Verification run
Passed targeted GTest filter including the new touch mapping regression.

## Open questions
Absolute mouse mapping on right-side monitors may deserve a separate follow-up if users report it.
