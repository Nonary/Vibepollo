#Requires -Version 5.1
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir,
    [string]$MsiPath = "",
    [string]$OutputName = "",
    [switch]$UninstallOnly,
    [switch]$SignWithSignPath,
    [switch]$DisableSignPath,
    [switch]$SkipSignPathIfNoToken,
    [switch]$NoInstallSignPathModuleIfMissing,
    [string]$SignPathApiToken = $env:SIGNPATH_API_TOKEN,
    [string]$SignPathOrganizationId = $(if ([string]::IsNullOrWhiteSpace($env:SIGNPATH_ORGANIZATION_ID)) { "1ba0e884-7ab4-43e6-aa84-9b2c7e3fba15" } else { $env:SIGNPATH_ORGANIZATION_ID }),
    [string]$SignPathProjectSlug = $(if ([string]::IsNullOrWhiteSpace($env:SIGNPATH_PROJECT_SLUG)) { "Vibepollo" } else { $env:SIGNPATH_PROJECT_SLUG }),
    [string]$SignPathSigningPolicySlug = $(if ([string]::IsNullOrWhiteSpace($env:SIGNPATH_SIGNING_POLICY_SLUG)) { "test-signing" } else { $env:SIGNPATH_SIGNING_POLICY_SLUG }),
    [string]$SignPathPeArtifactConfigurationSlug = $env:SIGNPATH_PE_ARTIFACT_CONFIGURATION_SLUG,
    [string]$SignPathMsiArtifactConfigurationSlug = $(if ([string]::IsNullOrWhiteSpace($env:SIGNPATH_MSI_ARTIFACT_CONFIGURATION_SLUG)) { "msi-file" } else { $env:SIGNPATH_MSI_ARTIFACT_CONFIGURATION_SLUG })
)

$ErrorActionPreference = "Stop"

function Resolve-PathStrict([string]$Path) {
    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    return $resolved.ProviderPath
}

function Find-LatestMsi([string]$Directory) {
    if (-not (Test-Path -LiteralPath $Directory)) {
        throw "MSI output directory does not exist: $Directory"
    }

    $candidate = Get-ChildItem -LiteralPath $Directory -Filter "*.msi" -File |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    if (-not $candidate) {
        throw "No MSI payload found in $Directory"
    }

    return $candidate.FullName
}

function Get-GitTagVersion([string]$RepoRoot) {
    $tagPatterns = @(
        '[0-9]*.[0-9]*.[0-9]*',
        'v[0-9]*.[0-9]*.[0-9]*'
    )

    try {
        foreach ($tagPattern in $tagPatterns) {
            $rawCandidates = git -C $RepoRoot tag --merged HEAD --sort=-version:refname --list $tagPattern 2>$null
            foreach ($candidate in $rawCandidates) {
                $rawTag = [string]$candidate
                if ([string]::IsNullOrWhiteSpace($rawTag)) {
                    continue
                }

                $rawTag = $rawTag.Trim()
                if ($rawTag -match '^v?(\d+)\.(\d+)\.(\d+)(?:([.-][0-9A-Za-z.-]+))?$') {
                    return @{
                        Tag = $rawTag
                        Major = [int]$matches[1]
                        Minor = [int]$matches[2]
                        Patch = [int]$matches[3]
                    }
                }
            }
        }
    } catch {
    }

    try {
        $rawTag = (git -C $RepoRoot describe --tags --abbrev=0 2>$null).Trim()
        if ($rawTag -match '^v?(\d+)\.(\d+)\.(\d+)(?:([.-][0-9A-Za-z.-]+))?$') {
            return @{
                Tag = $rawTag
                Major = [int]$matches[1]
                Minor = [int]$matches[2]
                Patch = [int]$matches[3]
            }
        }
    } catch {
    }

    return $null
}

function Get-GitInformationalVersion([string]$RepoRoot, [string]$fallbackTag) {
    try {
        $desc = (git -C $RepoRoot describe --tags --dirty --always 2>$null).Trim()
        if (-not [string]::IsNullOrWhiteSpace($desc)) {
            return $desc
        }
    } catch {
    }
    return $fallbackTag
}

function Get-MsiProductVersion([string]$MsiPath) {
    if ([string]::IsNullOrWhiteSpace($MsiPath) -or -not (Test-Path -LiteralPath $MsiPath)) {
        return $null
    }

    $installer = $null
    $database = $null
    $view = $null
    $record = $null
    try {
        $installer = New-Object -ComObject WindowsInstaller.Installer
        $database = $installer.GetType().InvokeMember("OpenDatabase", [System.Reflection.BindingFlags]::InvokeMethod, $null, $installer, @($MsiPath, 0))
        $view = $database.GetType().InvokeMember("OpenView", [System.Reflection.BindingFlags]::InvokeMethod, $null, $database, @("SELECT `Value` FROM `Property` WHERE `Property`='ProductVersion'"))
        $view.GetType().InvokeMember("Execute", [System.Reflection.BindingFlags]::InvokeMethod, $null, $view, $null) | Out-Null
        $record = $view.GetType().InvokeMember("Fetch", [System.Reflection.BindingFlags]::InvokeMethod, $null, $view, $null)
        if ($null -ne $record) {
            return [string]$record.StringData(1)
        }
    } catch {
    } finally {
        foreach ($com in @($record, $view, $database, $installer)) {
            if ($null -ne $com) {
                try {
                    [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($com)
                } catch {
                }
            }
        }
    }

    return $null
}

function Get-GeneratedWindowsFileVersion([string]$BuildDir) {
    $headerPath = Join-Path $BuildDir "generated_versioninfo\windows_versioninfo_generated.h"
    if (-not (Test-Path -LiteralPath $headerPath)) {
        return $null
    }

    $content = Get-Content -LiteralPath $headerPath -Raw
    $fields = @{}
    foreach ($name in @("MAJOR", "MINOR", "BUILD", "REVISION")) {
        if ($content -match "(?m)^\s*#define\s+RC_VERSION_$name\s+(\d+)\s*$") {
            $fields[$name] = [int]$matches[1]
        }
    }

    foreach ($name in @("MAJOR", "MINOR", "BUILD", "REVISION")) {
        if (-not $fields.ContainsKey($name)) {
            Write-Warning "Generated Windows version header is missing RC_VERSION_$name`: $headerPath"
            return $null
        }
    }

    return "{0}.{1}.{2}.{3}" -f $fields["MAJOR"], $fields["MINOR"], $fields["BUILD"], $fields["REVISION"]
}

function Resolve-CscPath {
    $candidates = @(
        (Join-Path $env:WINDIR "Microsoft.NET\Framework64\v4.0.30319\csc.exe"),
        (Join-Path $env:WINDIR "Microsoft.NET\Framework\v4.0.30319\csc.exe"),
        "D:/Software/Visual Studio/MSBuild/Current/Bin/Roslyn/csc.exe"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-PathStrict $candidate)
        }
    }

    throw "Could not locate a C# compiler (csc.exe)."
}

function Invoke-SignPathForArtifact(
    [string]$ArtifactPath,
    [string]$Description
) {
    $signPathScript = Resolve-PathStrict (Join-Path $repoRoot "scripts\signpath_sign.ps1")

    $signArgs = @{
        InputArtifactPath = $ArtifactPath
        OutputArtifactPath = $ArtifactPath
        ApiToken = $SignPathApiToken
        OrganizationId = $SignPathOrganizationId
        ProjectSlug = $SignPathProjectSlug
        SigningPolicySlug = $SignPathSigningPolicySlug
        PeArtifactConfigurationSlug = $SignPathPeArtifactConfigurationSlug
        MsiArtifactConfigurationSlug = $SignPathMsiArtifactConfigurationSlug
        Description = $Description
        WaitForCompletionTimeoutInSeconds = 1800
    }

    if ($SkipSignPathIfNoToken) {
        $signArgs.SkipIfMissingToken = $true
    }

    if (-not $NoInstallSignPathModuleIfMissing) {
        $signArgs.InstallModuleIfMissing = $true
    }

    & $signPathScript @signArgs
}

$scriptDir = Split-Path -Parent $PSCommandPath
$repoRoot = Resolve-PathStrict (Join-Path $scriptDir "..\..\..")
$buildRoot = Resolve-PathStrict $BuildDir
$artifactDir = Join-Path $buildRoot "cpack_artifacts"

if (-not $UninstallOnly) {
    if ([string]::IsNullOrWhiteSpace($MsiPath)) {
        $MsiPath = Find-LatestMsi -Directory $artifactDir
    } else {
        $MsiPath = Resolve-PathStrict $MsiPath
    }
}

$sourceFile = Resolve-PathStrict (Join-Path $scriptDir "VibeshineInstaller.cs")
$manifestFile = Resolve-PathStrict (Join-Path $scriptDir "app.manifest")
# Keep the branded product icon on the bootstrapper; the installer now uses
# explicit process/window shell metadata to stay distinct from the installed app.
$iconPath = Resolve-PathStrict (Join-Path $repoRoot "apollo.ico")
$licensePath = Resolve-PathStrict (Join-Path $repoRoot "LICENSE")
$cscPath = Resolve-CscPath
$frameworkRoot = Resolve-PathStrict "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
$wpfRoot = Resolve-PathStrict (Join-Path $frameworkRoot "WPF")

if (-not (Test-Path -LiteralPath $artifactDir)) {
    New-Item -ItemType Directory -Path $artifactDir -Force | Out-Null
}

if ([string]::IsNullOrWhiteSpace($OutputName)) {
    if ($UninstallOnly) {
        $OutputName = "uninstall.exe"
    } else {
        $OutputName = "VibepolloSetup.exe"
    }
}

$outputPath = Join-Path $artifactDir $OutputName

$tagVersion = Get-GitTagVersion -RepoRoot $repoRoot
$fallbackTag = if ($null -eq $tagVersion) { "" } else { $tagVersion.Tag }
$informationalVersion = Get-GitInformationalVersion -RepoRoot $repoRoot -fallbackTag $fallbackTag
$generatedFileVersion = Get-GeneratedWindowsFileVersion -BuildDir $BuildDir
$assemblyVersion = $generatedFileVersion

if (-not $UninstallOnly) {
    $msiProductVersion = Get-MsiProductVersion -MsiPath $MsiPath
    if ([string]::IsNullOrWhiteSpace($assemblyVersion) -and -not [string]::IsNullOrWhiteSpace($msiProductVersion) -and $msiProductVersion -match '^(\d+)\.(\d+)\.(\d+)(?:\.(\d+))?') {
        $revision = if ([string]::IsNullOrWhiteSpace($matches[4])) { 0 } else { [int]$matches[4] }
        $assemblyVersion = "{0}.{1}.{2}.{3}" -f [int]$matches[1], [int]$matches[2], [int]$matches[3], $revision
        if ([string]::IsNullOrWhiteSpace($informationalVersion)) {
            $informationalVersion = $msiProductVersion
        }
    }
}

if ([string]::IsNullOrWhiteSpace($assemblyVersion) -and $null -ne $tagVersion) {
    $assemblyVersion = "{0}.{1}.{2}.0" -f $tagVersion.Major, $tagVersion.Minor, $tagVersion.Patch
}

if ([string]::IsNullOrWhiteSpace($assemblyVersion)) {
    $assemblyVersion = "0.0.0.0"
    Write-Warning "Could not determine installer assembly version from MSI payload or git tag. Falling back to $assemblyVersion."
}

if ([string]::IsNullOrWhiteSpace($informationalVersion)) {
    $informationalVersion = $assemblyVersion
}
if ($UninstallOnly) {
    $assemblyInfoPath = Join-Path $artifactDir "VibepolloUninstall.AssemblyInfo.cs"
    $assemblyTitle = "Vibepollo Uninstaller"
} else {
    $assemblyInfoPath = Join-Path $artifactDir "VibepolloInstaller.AssemblyInfo.cs"
    $assemblyTitle = "Vibepollo Installer"
}
$assemblyInfoContent = @(
    "using System.Reflection;",
    "[assembly: AssemblyTitle(""$assemblyTitle"")]",
    "[assembly: AssemblyDescription(""$assemblyTitle"")]",
    "[assembly: AssemblyProduct(""$assemblyTitle"")]",
    "[assembly: AssemblyCompany(""Nonary"")]",
    "[assembly: AssemblyVersion(""$assemblyVersion"")]",
    "[assembly: AssemblyFileVersion(""$assemblyVersion"")]",
    "[assembly: AssemblyInformationalVersion(""$informationalVersion"")]"
)
Set-Content -Path $assemblyInfoPath -Value $assemblyInfoContent -Encoding UTF8

# Only sign when explicitly requested with -SignWithSignPath. Mere token
# presence must NOT trigger signing: release artifacts are signed by SignPath
# origin verification in CI (docs/signpath/), and a runner-local signature
# cannot satisfy origin verification. -DisableSignPath always wins.
$shouldSignWithSignPath = -not $DisableSignPath -and $SignWithSignPath

if (-not $shouldSignWithSignPath) {
    Write-Host "[bootstrapper] SignPath signing disabled; output will be unsigned (CI signs release artifacts via SignPath origin verification)."
}

if ($shouldSignWithSignPath -and -not $UninstallOnly) {
    Invoke-SignPathForArtifact `
        -ArtifactPath $MsiPath `
        -Description "Vibepollo MSI payload $informationalVersion"
}

$references = @(
    (Join-Path $frameworkRoot "System.dll"),
    (Join-Path $frameworkRoot "System.Core.dll"),
    (Join-Path $frameworkRoot "System.Data.dll"),
    (Join-Path $frameworkRoot "System.Xml.dll"),
    (Join-Path $frameworkRoot "System.Xaml.dll"),
    (Join-Path $frameworkRoot "System.Windows.Forms.dll"),
    (Join-Path $wpfRoot "WindowsBase.dll"),
    (Join-Path $wpfRoot "PresentationCore.dll"),
    (Join-Path $wpfRoot "PresentationFramework.dll")
)

$args = @(
    "/nologo",
    "/target:winexe",
    "/optimize+",
    "/utf8output",
    "/out:$outputPath",
    "/win32manifest:$manifestFile",
    "/win32icon:$iconPath",
    "/resource:$licensePath,License.txt"
)

if ($UninstallOnly) {
    $args += "/define:UNINSTALL_ONLY"
} else {
    $args += "/resource:$MsiPath,Payload.msi"
}

foreach ($reference in $references) {
    if (-not (Test-Path -LiteralPath $reference)) {
        throw "Missing reference assembly: $reference"
    }
    $args += "/reference:$reference"
}

$args += $assemblyInfoPath
$args += $sourceFile

if ($UninstallOnly) {
    Write-Host "[bootstrapper] Building lightweight uninstaller EXE..."
} else {
    Write-Host "[bootstrapper] Building custom installer EXE..."
    Write-Host "[bootstrapper] Input MSI: $MsiPath"
}
Write-Host "[bootstrapper] Output EXE: $outputPath"
Write-Host "[bootstrapper] Compiler: $cscPath"
Write-Host "[bootstrapper] Version: $assemblyVersion ($informationalVersion)"

& $cscPath @args
if ($LASTEXITCODE -ne 0) {
    throw "C# compiler failed with exit code $LASTEXITCODE"
}

if ($shouldSignWithSignPath) {
    $artifactDescription = if ($UninstallOnly) {
        "Vibepollo uninstaller $informationalVersion"
    } else {
        "Vibepollo setup executable $informationalVersion"
    }
    Invoke-SignPathForArtifact `
        -ArtifactPath $outputPath `
        -Description $artifactDescription
}

if ($UninstallOnly) {
    Write-Host "[bootstrapper] Lightweight uninstaller build complete."
} else {
    Write-Host "[bootstrapper] Custom installer build complete."
}
