# Packet 01-layout-automation: Layout Automation Flow

## Objective
Find the code path that handles virtual display creation/reconnect, display automation enablement, and monitor layout application/restoration.

## Context
The user reports that disabling global display automation still allows reconnects to disrupt the host monitor layout. Desired behavior is that a virtual display acts like a plugged-in display and preserves the user-arranged extended layout.

## Sources
- Start with `rg "display automation|display_automation|virtual display|virtual_display|layout|restore|reconnect" src`
- Inspect only nearby code needed to trace the call path.

## Ownership
Read-only explorer agent.

## Do
- Identify entry points and config flags controlling display automation.
- Trace whether virtual display connection applies layout/resolution even when automation is disabled.
- Return likely files/symbols the parent should patch.
- Cite file paths and line numbers where possible.

## Do not
- Edit, create, delete, move, or rename files.
- Run broad tests, formatting, installs, packaging, or destructive commands.
- Duplicate touch coordinate investigation.

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
