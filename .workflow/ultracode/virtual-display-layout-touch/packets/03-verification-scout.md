# Packet 03-verification-scout: Verification Scout

## Objective
Find the narrowest existing verification path for changes in display layout automation and touch/pen coordinate mapping.

## Context
The repo instructions prohibit broad test suites by default. We need a focused test/build command for the likely files touched.

## Sources
- Start with `rg "display|touch|pen|input|virtual" tests src -g "*test*" -g "CMakeLists.txt" -g "package.json"`
- Inspect build/task config only as needed.

## Ownership
Read-only explorer agent.

## Do
- Identify relevant test files, if any.
- Identify the smallest compile-only build command for affected native code.
- Note whether web UI build is relevant.
- Return exact commands with PowerShell-compatible syntax.

## Do not
- Edit, create, delete, move, or rename files.
- Run tests unless trivially read-only and explicitly needed for discovery.
- Run full suite, packaging, installs, or formatting.

## Expected output
Focused verification recommendation with evidence.

## Verification
Read-only source/config evidence only.

## Handoff format
- Task answered:
- Key findings:
- Relevant files:
- Important symbols:
- Evidence:
- Confidence:
- Open questions:
- Suggested next investigation:
