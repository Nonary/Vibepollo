# Virtual Display Layout And Touch

## Goal
Fix the reported Vibeshine virtual display behavior so reconnecting with a virtual screen preserves the user-arranged extended desktop layout when display automation is off, and touch/pen input targets the virtual screen correctly when it is positioned to the right of the physical display.

## Success criteria
- Desktop app launch leaves the current desktop arrangement unchanged when no virtual display workflow is requested.
- Virtual desktop launch behaves like plugging in another display and does not overwrite the user's existing extended layout when automation is disabled.
- Touch/pen coordinate mapping accounts for virtual screens placed on either side of the primary display.
- The change is verified with the smallest relevant build/test command available in this repo.

## Current context
- User reports layout restore differs from Apollo/Vibepollo expectations.
- Turning off global display automation improves behavior but reconnecting still disrupts monitor layout.
- Touch/pen works when the virtual screen is on the left, but lands on the physical display when the virtual screen is on the right.

## Constraints
- Work from `D:\sources\sunshine`.
- PowerShell shell: use `rg`, not `sed`/`grep`; avoid bash heredocs.
- Do not run the full test suite by default.
- Do not package installers or symbol bundles unless producing an installable build.
- Do not commit, push, publish, or deploy.

## Risk level
Medium. The likely blast radius is display device layout/application and input coordinate translation. Mistakes could disrupt user monitor arrangements or input routing.

## Approval gates
No approval needed for read-only investigation, local code edits, targeted tests, or compile-only build verification. Approval would be required for destructive operations, publishing, broad codemods, or installable packaging.

## Mode
Delegated mode. Power level: high (`5.5 high`). Planned agent budget: 3 read-only explorer agents in one wave, with parent implementation and verification.

## Work packets
- `01-layout-automation`: trace virtual display enable/reconnect and display automation/layout application paths.
- `02-touch-coordinate-mapping`: trace touch/pen coordinate mapping for extended desktop layouts.
- `03-verification-scout`: find narrow tests/build commands and relevant existing test coverage.

## Eval contract
- Outcome: targeted product code fix for virtual-display layout preservation and/or touch coordinate mapping, backed by repository evidence.
- Shared surfaces: display device/layout automation code, virtual display integration, touch/pen input injection path.
- Required checks: targeted unit/test command if available; otherwise compile-only CMake build for affected native code.
- Blocking conditions: no clear code path found, or fix requires external driver changes outside this repo.
- Handoff evidence: file paths, symbols, line references, final diff, and verification output summary.

## Integration policy
The parent session owns all edits. Read-only agent findings are accepted only when backed by file/symbol evidence. Conflicts are resolved by inspecting source and choosing the smallest change consistent with existing patterns.

## Verification plan
- Inspect final diff.
- Run the narrowest relevant test command if one exists.
- If no narrow test exists, run compile-only native build for the current build tree/config with MSYS2 path prepended.
- Do not package installers.

## Completion criteria
- Root cause identified with source evidence.
- Code change implemented and reviewed.
- Required verification run or explicitly skipped with reason.
- `integration.md`, `state.json`, and `final-report.md` updated.
