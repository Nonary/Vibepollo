# Paired-client sleep protocol (version 1)

This host extension supports a client-side “Put PC to Sleep” control. It does not add an application to `/applist`. Compatible Moonlight clients display their own synthetic tile only when paired HTTPS `/serverinfo` advertises `PowerActionVersion=1`, `SleepSupported=1` and `SleepAllowed=1`. This metadata contains no token. Stock clients ignore the extra fields and retain their existing app list.

The companion client patch confirms the action, obtains a fresh token, and sends one empty POST without creating a streaming `Session`, calling `/launch` or `/resume`, or starting RTSP/video/audio/input capture. The tile cannot be made a direct-launch app. Refusal, timeout or lost acknowledgement requires another user confirmation; the client never retries automatically.

Native sleep currently supports Windows hosts whose power capabilities include
S1/S2/S3 or Modern Standby. Linux and macOS report unsupported. Linux's machine
host would need a separately reviewed privileged broker operation. No configurable
command, shell execution, automatic elevation, hibernate or shutdown is exposed.

## Administrator opt-in

Every new or existing paired client defaults to `allow_sleep: false`. Being
paired or allowed to stream does not grant sleep permission. An authenticated
administrator can use the existing JSON `POST /api/clients/update` Web UI API:

```json
{"uuid":"<paired-client-uuid>","allow_sleep":true}
```

The grant is stored in the paired device record and returned by the paired
client listing API. Only a successful `allow_sleep_updated` result confirms
durable saving. Set `allow_sleep` to `false` to revoke it. A failed grant cannot
expand authority; a failed revocation denies access in the current process but
must be repaired before restart to persist that revocation. The paired device editor exposes this as “Allow this device to put the PC to sleep”. The checkbox is independent of streaming/native permission masks and defaults off.

## Client discovery and action

Use the existing paired-client HTTPS port and client certificate for both
requests. Plain HTTP exposes neither endpoint. Client UUID query arguments do
not establish identity; the host resolves the current exact enabled certificate.

1. After the user deliberately selects and confirms sleep, send
   `GET /power-actions/v1`. The XML response includes `PowerActionVersion=1`,
   `SleepSupported`, `SleepAllowed`, `SleepAvailable` and
   `SleepActiveSessionPolicy=idle-only`. Offer the action only when all three
   flags are `1`. Availability is an observation and is checked again on POST.
2. Use the response's `SleepToken` in
   `POST /power-actions/v1/sleep?token=<SleepToken>` with an empty body. The token
   is random, bound to that paired identity, expires after 30 seconds, and can
   be used once. A new discovery request replaces that client's previous token.
   Additional arguments, duplicate token parameters, bodies and browser Origin
   headers are rejected. Do not send a shell command or an application ID.
3. The host refuses sleep if any application remains retained for Resume,
   any RTSP/WebRTC session is active, pending, starting or tearing down, or any
   Remote Input/Remote Monitor owner remains. End those sessions first. The
   host never terminates someone else's application or session for this action.
4. HTTP and XML `202` with `SleepAccepted=1` means the request was accepted,
   **not** that the OS completed sleep. The complete response is flushed before
   the native call. A failed or timed-out flush prevents the call, and a final
   live certificate/permission check can still prevent it. Native failures are
   logged by the host. The native request preserves wake events.

The client must leave stream state untouched, dismiss its action UI after
acceptance, and tolerate subsequent discovery reporting the host offline. It
must not automatically retry on timeout, reconnect, wake or failure. A retry
requires a new user confirmation and a fresh token. Consumed tokens cannot
execute again, including after wake. Tokens are process-local and never survive
a host restart. Refusals include `401` for missing/revoked authority, `403` for
no sleep grant, `400` for an invalid request, `409` for busy/stale/used tokens,
`501` for unsupported native sleep and `503` for occupied mutation admission.

## Validation limits

Portable component tests exercise the production certificate store, live grant
revocation, protocol parsing, response XML/status, token expiry/replay, idle
policy and response-flush ordering. A Win32 API shim exercises the actual native
adapter's thread privilege handling and fixed sleep parameters. These do not
prove Windows SDK compilation, firmware/Modern Standby behavior, service-token
permissions, wake-on-LAN, or a complete native client/host sleep-and-reconnect workflow.

Before considering the complete feature shipped, build on native Windows and
test a compatible client's confirmation, sleep/wake/reconnect cycle, disabled
and revoked clients, all busy states, lost acceptance responses and replay
after wake. Keep an independent way to wake the test machine. No suspend or
service/device changes are required by the portable tests.

Native API contracts:
[SetSuspendState](https://learn.microsoft.com/en-us/windows/win32/api/powrprof/nf-powrprof-setsuspendstate),
[GetPwrCapabilities](https://learn.microsoft.com/en-us/windows/win32/api/powrprof/nf-powrprof-getpwrcapabilities).

## Companion client acceptance

The client must build on native Qt/Windows before release. Verify mouse, keyboard and gamepad confirmation/cancellation, disappearance after permission revocation, unchanged app/direct-launch behavior against stock hosts, and display of HTTP/XML acceptance versus unknown outcome after a lost response. A local fake network transport can prove request method, path, exact token query, empty body, no redirects, no stream requests and no application retry, but cannot prove native TLS handshakes, service-token rights, firmware sleep or wake/reconnect. No actual power transition is part of portable validation.
