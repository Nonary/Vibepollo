The console harness compiles the actual connector lifecycle, pipe connection,
data mapper and environment scope classes. It uses real OS named pipes, with
boundary substitutes for Playnite, WPF dispatch, logging and Windows client
authentication. It checks acceptance during Stop, queued launches across a
restart, stalled writers, cancellation of pending UI queries, snapshot ordering
and graceful shutdown handoff, waveform preparation routing and launcher
readiness acknowledgments. It does not validate Windows pipe ACLs or native
WPF/Playnite runtime behavior.

Run with .NET SDK 8 or later:

```sh
dotnet run --project tests/playnite_connector/ConnectorLifecycle.csproj -c Release
```

CTest runs the harness when a suitable dotnet SDK is available.
