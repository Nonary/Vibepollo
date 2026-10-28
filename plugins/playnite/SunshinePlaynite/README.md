# Vibepollo Playnite Connector

This is the compiled Playnite 10 replacement for the retired PowerShell script extension.
Playnite 11 uses a separate, incompatible SDK and will require its own connector port after release.

## Build

Build `SunshinePlaynite.csproj` in Release mode, then package the contents of
`bin/Release/net462` as a Playnite extension with Toolbox:

```powershell
dotnet build SunshinePlaynite.csproj -c Release
Toolbox.exe pack .\bin\Release\net462 .
```

For local testing, add `bin/Release/net462` as a developer plugin in Playnite's
`Settings -> For developers` page. Remove the old PowerShell extension before
testing or installing this compiled replacement.

The connector settings are available in Playnite Desktop Mode under
`Add-ons -> Extension settings -> Generic -> Vibepollo Playnite Connector`.
Settings control whether the connector runs, whether library changes notify
Vibepollo, and whether verbose diagnostic logging is enabled.

Waveform haptics retains the connector readiness and environment acknowledgments
used by the native launcher. Requested hooks are prepared in `OnGameStarting`;
failed preparation cancels the launch and notifies the launcher. Steam uses the
native helper, including when Playnite runs as a 32-bit process.

## Client authentication

The pipe ACL admits only the Playnite user and LocalSystem, and denies network
clients. Each hello must declare a supported role and the kernel-reported PID
of the connected data-pipe client. Normally the connector also checks the
client executable against that role.

Normal-user Playnite cannot inspect the SYSTEM service's process image. When
image inspection returns access denied, only the `sunshine` role may instead
use the connected pipe client's LocalSystem user SID. Windows identifies the
sender of the hello that was read; the claimed PID, executable name, or an
administrator identity alone cannot authorize this fallback. A successfully
inspected but wrong executable is still rejected, including for SYSTEM clients.
The LocalSystem principal is trusted because it already controls the host and
can modify the Playnite user's processes; this fallback does not authenticate
an inaccessible image name. Launchers always require executable inspection.

Native Playnite IPC connections request `SECURITY_IDENTIFICATION`, so a normal
Playnite server can query the service identity without `SeImpersonatePrivilege`
and cannot use the connection token to act as SYSTEM. Other helper IPC keeps
its existing security quality of service.

Windows runtime validation requires a SYSTEM service host connecting to
non-elevated Playnite: verify library synchronization and normal launcher
connections, then verify an unrelated user client, a mismatched PID, and a
launcher with an inaccessible process image are rejected. The portable harness
checks the actual authentication policy, but cannot exercise Windows token,
ACL, impersonation restoration, or native pipe behavior.
