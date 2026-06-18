# Orchestration

## Parent critical path
Keep implementation local: inspect the agent results, verify the implicated call path directly, patch the smallest relevant files, and run targeted verification.

## Packets
- `01-layout-automation`: read-only; owner `read-only-agent`; find layout/automation call path and likely reconnect overwrite point.
- `02-touch-coordinate-mapping`: read-only; owner `read-only-agent`; find touch/pen coordinate translation path and right-of-primary assumptions.
- `03-verification-scout`: read-only; owner `read-only-agent`; find narrow tests/build commands and test gaps.

## Delegation
Native Codex agents are available. One discovery wave with three explorer agents is planned.

## Agents
- Explorer 1: layout automation flow.
- Explorer 2: touch/pen coordinate mapping.
- Explorer 3: verification and tests.

## Delegation limits
Power level high, agent budget 3 of maximum 5 without approval. Agents are read-only and must not edit or run broad tests.

## Wait points
Wait for all three explorer agents before making code decisions, per the read-only agents workflow.

## Fallback
If an agent fails or returns no evidence, the parent session performs the missing targeted inspection locally.

## Verification order
1. Inspect final diff.
2. Run relevant narrow tests if available.
3. Otherwise run compile-only native build for affected code.
4. Update final artifacts.
