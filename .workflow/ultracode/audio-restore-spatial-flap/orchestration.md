# Orchestration

## Parent critical path
Inspect the audio control implementation, integrate agent evidence, implement the smallest fix, and verify compilation.

## Packets
- `01-audio-restore-flow`: read-only trace of capture/restore flow from session start to stop.
- `02-spatial-flap-hypothesis`: read-only inspection of Windows endpoint write behavior and possible rapid repeated writes.
- `03-implementation`: parent-owned source edit and verification.

## Delegation
Use two read-only explorer agents. No write-capable agents.

## Agents
- Agent 1: audio restore flow finder.
- Agent 2: Windows audio control/spatial flap hypothesis finder.

## Delegation limits
Agent count: 2. Wave count: 1. Fan-out stays below 5.

## Wait points
Wait for both read-only agents before making source edits.

## Fallback
If an agent fails, continue with parent source inspection and record the missing result.

## Verification order
Inspect diff, run targeted native build, update final report.
