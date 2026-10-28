The console harness compiles the actual connector lifecycle, pipe connection,
data mapper and environment scope classes. It uses real OS named pipes, with
boundary substitutes for Playnite, WPF dispatch, logging and Windows client
authentication. It also compiles the actual client authentication policy and
checks the SYSTEM fallback, denied/failed token inspection, PID spoofing, role
mismatches and normal executable validation. On Linux the Python runner also
compiles the production native pipe-open and Playnite connection code with
syscall boundaries mocked to check scoped identification flags. It checks acceptance during Stop, queued launches across a
restart, stalled writers, cancellation of pending UI queries, snapshot ordering
and graceful shutdown handoff, waveform preparation routing and launcher
readiness acknowledgments. It does not validate Windows pipe ACLs or native
WPF/Playnite runtime behavior.

Run with .NET SDK 8 or later:

```sh
dotnet run --project tests/playnite_connector/ConnectorLifecycle.csproj -c Release
```

CTest runs the harness when a suitable dotnet SDK is available.
