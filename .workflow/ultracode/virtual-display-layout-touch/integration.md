# Integration

## Accepted
- Layout automation finding: virtual-display sessions forced disabled automation back to `ensure_active`; this should remain active only for the intentional exclusive default.
- Touch mapping finding: Windows touch/pen coordinates were offset-subtracted before Windows injection offset was applied.
- Verification finding: use focused `test_sunshine` filters plus compile-only `sunshine` build.

## Rejected
None yet.

## Conflicts
None yet.

## Decisions
- Replaced disabled-to-`ensure_active` overrides with an exclusive-layout-only `ensure_only_display` override in both display request helper constructors.
- Kept Linux touch/pen normalization desktop-relative, but changed non-Linux normalization to monitor-local.
- Added regression tests for virtual-display disabled request skipping and touch coordinate normalization.

## Final changes
- `src/platform/windows/display_helper_request_helpers.cpp`
- `src/input.cpp`
- `src/input.h`
- `tests/unit/test_display_helper_session_deferral.cpp`
- `tests/unit/test_input.cpp`

## Verification still needed
- Complete.

## Remaining risks
- Runtime verification on a Windows host with an actual right-side extended virtual display is still useful because OS display persistence is external to unit tests.
