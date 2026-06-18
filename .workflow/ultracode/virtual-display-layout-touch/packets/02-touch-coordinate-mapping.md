# Packet 02-touch-coordinate-mapping: Touch Coordinate Mapping

## Objective
Find the touch/pen input path for virtual display sessions and identify why a virtual screen positioned to the right could map touch to the physical display while positioning it to the left works.

## Context
The user reports touch/pen input on the virtual screen lands on the normal screen when the virtual screen is arranged to the right, but works when the virtual screen is on the left.

## Sources
- Start with `rg "touch|pen|tablet|absolute|coordinate|SendInput|Inject|pointer|virtual screen" src`
- Inspect only nearby code needed to trace coordinate conversion and display bounds.

## Ownership
Read-only explorer agent.

## Do
- Identify where client touch/pen coordinates are converted into host coordinates.
- Check for assumptions about display origin, primary monitor, positive offsets, virtual desktop bounds, or normalization.
- Return likely files/symbols the parent should patch.
- Cite file paths and line numbers where possible.

## Do not
- Edit, create, delete, move, or rename files.
- Run broad tests, formatting, installs, packaging, or destructive commands.
- Duplicate layout automation investigation.

## Expected output
Concise findings with evidence and a recommended parent action.

## Verification
Read-only source evidence only.

## Handoff format
- Task answered:
- Key findings:
- Relevant files:
- Important symbols:
- Evidence:
- Confidence:
- Open questions:
- Suggested next investigation:
