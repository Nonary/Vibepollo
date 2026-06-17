# Packet 01-audio-restore-flow: Audio Restore Flow

## Objective
Trace how Vibeshine captures the pre-session default audio output, switches to the configured stream sink, and restores output at session end.

## Context
The user reports that after exiting a session Vibeshine never switches back to the correct audio output.

## Sources
- `src/audio.cpp`
- `src/audio.h`
- Windows platform audio files discovered with `rg "audio_control|reset_default_device|set_default_device"`

## Ownership
Read-only agent.

## Do
- Inspect only relevant audio/session source files.
- Cite file paths and line numbers where possible.
- Identify exact symbols and ordering for capture, switch, and restore.
- Note suspicious lifetime or state handling.

## Do not
- Edit, create, delete, move, or rename files.
- Install dependencies or run broad tests.
- Take over implementation.

## Expected output
- Summary of restore flow.
- Evidence with file paths and symbols.
- Likely failure points.
- Recommended parent action.

## Verification
Inspection only.

## Handoff format
Use `results/01-audio-restore-flow.md` schema.
