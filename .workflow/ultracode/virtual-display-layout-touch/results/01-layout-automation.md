# Result 01-layout-automation: Layout Automation Flow

## Summary
Global display automation disabled via `dd_configuration_option=disabled` was not correctly scoped for virtual-display sessions. Exclusive virtual displays intentionally need display-helper exclusivity, but extended virtual displays should not force helper topology/layout changes when automation is disabled.

## Evidence
- `src/platform/windows/display_helper_request_helpers.cpp` had disabled-to-`ensure_active` overrides in both `SessionDisplayConfigurationHelper` and `SessionMonitorPositionHelper`.
- The replacement override only applies to `virtual_display_layout_e::exclusive`, mapping disabled automation to `ensure_only_display` so exclusivity remains intact.
- Extended virtual display layouts now keep `configuration_disabled_tag_t`, causing request construction to skip display-helper apply.

## Handoff
Handoff:
- Summary: Keep exclusive virtual displays exclusive by default, but stop forcing display automation back on for extended virtual display sessions when disabled globally.
- Changed surfaces: `src/platform/windows/display_helper_request_helpers.cpp`, `tests/unit/test_display_helper_session_deferral.cpp`.
- Contracts satisfied: Display automation disabled skips extended virtual display request construction while exclusive virtual display request construction remains active.
- Assumptions: Virtual display creation itself may still occur; only layout/resolution/topology automation is suppressed.
- Local checks: Targeted GTest filter passed.
- Integration evidence: Added `DisplayHelperRequestHelpers.AppliesExclusiveVirtualDisplayWhenDisplayConfigurationDisabled` and `DisplayHelperRequestHelpers.SkipsExtendedVirtualDisplayWhenDisplayConfigurationDisabled`.
- Risks: Recovery paths may still attempt to build a request and log a warning when disabled, but no apply is produced.

## Files changed
- `src/platform/windows/display_helper_request_helpers.cpp`
- `tests/unit/test_display_helper_session_deferral.cpp`

## Decisions
Narrow the override by virtual display layout rather than adding new reconnect gates. This keeps disabled semantics centralized in request construction.

## Risks
Users with display automation disabled still get exclusive virtual display behavior by default. Extended virtual display layouts will not get automatic helper topology/layout changes while disabled.

## Verification run
Passed targeted GTest filter including the new virtual-display disabled test.

## Open questions
None blocking.
