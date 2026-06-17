# Packet 02-spatial-flap-hypothesis: Spatial Flap Hypothesis

## Objective
Find Windows audio endpoint code that could repeatedly set default audio devices during first stream startup and cause Sound Settings spatial selection to churn.

## Context
The user reports the first stream causes the Windows Sound Settings "spatial" section selection to change several times per second, with service CPU over 30%. Disconnecting and reconnecting resolves it.

## Sources
- Windows audio platform files discovered with `rg "IPolicyConfig|SetDefaultEndpoint|IMMDevice|spatial|audio_control"`
- Related logs or retry loops near endpoint switching.

## Ownership
Read-only agent.

## Do
- Inspect Windows audio control implementation and nearby retry/poll loops.
- Cite line numbers and symbols.
- Identify whether endpoint writes are idempotent or repeated.
- Point out likely relationship to spatial audio UI churn.

## Do not
- Edit, create, delete, move, or rename files.
- Install dependencies or run broad tests.
- Take over implementation.

## Expected output
- Summary of endpoint write behavior.
- Evidence with file paths and symbols.
- Likely cause and narrow mitigation.
- Risks or unknowns.

## Verification
Inspection only.

## Handoff format
Use `results/02-spatial-flap-hypothesis.md` schema.
