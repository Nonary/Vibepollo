# Remote Microphone Support

This feature adds a working host-side remote microphone path for Vibepollo, focused on Windows hosts and VB-CABLE integration.

## Overview

The microphone path is:

1. A compatible Moonlight or Artemis client captures local microphone audio.
2. The client sends encrypted or unencrypted microphone packets to Vibepollo on the dedicated microphone stream.
3. Vibepollo receives the packets, decrypts them when needed, and decodes the Opus frames on the host.
4. Vibepollo renders the decoded PCM into the VB-CABLE playback endpoint `CABLE Input`.
5. Host applications consume that audio from the VB-CABLE recording endpoint `CABLE Output`.

This keeps the host-side application flow simple: Vibepollo writes into VB-CABLE, and games, chat apps, or capture tools use `CABLE Output` as the microphone.

## What Changed

The working implementation includes:

- Dedicated microphone session handling in the stream path, including packet receive, optional decryption, and per-session lifecycle management.
- Windows microphone backend initialization and teardown that stays alive for the full remote microphone session.
- A VB-CABLE-backed Windows render path that auto-detects `CABLE Input`, creates a shared-mode WASAPI render client, decodes Opus microphone frames, and writes PCM into the render buffer.
- Host-side recovery for recoverable WASAPI failures such as device invalidation or audio service restarts.
- A Remote Microphone Debug panel in the troubleshooting UI that shows packet arrival, decode status, render status, signal detection, counters, and recent mic events.

## Key Files

- `src/stream.cpp`: microphone socket handling, session startup/shutdown, and packet routing.
- `src/audio.cpp`: shared microphone debug state and persistent audio context ownership for the redirect device.
- `src/platform/windows/audio.cpp`: Windows microphone backend selection and redirect device ownership.
- `src/platform/windows/apollo_vmic.cpp`: VB-CABLE backend wrapper.
- `src/platform/windows/mic_write.cpp`: device discovery, WASAPI initialization, Opus decode, and VB-CABLE rendering.
- `src_assets/common/assets/web/Troubleshooting.vue`: Remote Microphone Debug UI.

## Windows Requirements

- Install VB-CABLE on the host.
- Ensure the playback endpoint `CABLE Input` exists and is enabled.
- In host applications, select `CABLE Output` as the microphone/recording source.
- Enable `stream_mic` in Vibepollo.
- Use a client build that supports microphone redirection.

## Compatible Client Builds

Microphone passthrough requires matching client support:

- Desktop: [logabell/moonlight-qt-mic](https://github.com/logabell/moonlight-qt-mic)
- Android (Artemis): [logabell/moonlight-android](https://github.com/logabell/moonlight-android)
- Shared protocol library: [logabell/moonlight-common-c-mic](https://github.com/logabell/moonlight-common-c-mic)

## Configuration Notes

- `stream_mic` enables the host microphone redirect path.
- `mic_backend` defaults to `vb_cable` on Windows in this fork.
- On Windows, Vibepollo currently auto-detects the VB-CABLE render endpoint and standardizes on the VB-CABLE backend.
- `mic_device` is mainly relevant on non-Windows platforms. The Windows path currently targets VB-CABLE automatically.

## Debugging

The Troubleshooting page on Windows exposes a Remote Microphone Debug panel that shows:

- whether the client is sending packets
- whether Vibepollo is decoding microphone frames
- whether Vibepollo is rendering into VB-CABLE
- whether non-silent input is being detected
- the most recent mic errors and recent mic events

This view is intended to quickly separate client capture problems from host decode/render problems.
