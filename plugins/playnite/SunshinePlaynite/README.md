# Sunshine Playnite Connector

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
`Add-ons -> Extension settings -> Generic -> Sunshine Playnite Connector`.
Settings control whether the connector runs, whether library changes notify
Vibeshine, and whether verbose diagnostic logging is enabled.
