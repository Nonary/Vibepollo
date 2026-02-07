#Requires -Version 5.1
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir,
    [string]$MsiPath = "",
    [string]$OutputName = "",
    [switch]$UninstallOnly
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
    try {
        $rawTag = (git -C $RepoRoot describe --tags --abbrev=0 2>$null).Trim()
        if ([string]::IsNullOrWhiteSpace($rawTag)) {
            return $null
        }

        if ($rawTag -match '^v?(\d+)\.(\d+)\.(\d+)$') {
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
$iconPath = Resolve-PathStrict (Join-Path $repoRoot "sunshine.ico")
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
        $OutputName = "VibeshineSetup.exe"
    }
}

$outputPath = Join-Path $artifactDir $OutputName

$tagVersion = Get-GitTagVersion -RepoRoot $repoRoot
if ($null -eq $tagVersion) {
    throw "Could not determine installer version from git tag. Expected a tag like v1.2.3 or 1.2.3."
}

$assemblyVersion = "{0}.{1}.{2}.0" -f $tagVersion.Major, $tagVersion.Minor, $tagVersion.Patch
$informationalVersion = Get-GitInformationalVersion -RepoRoot $repoRoot -fallbackTag $tagVersion.Tag
if ($UninstallOnly) {
    $assemblyInfoPath = Join-Path $artifactDir "VibeshineUninstall.AssemblyInfo.cs"
    $assemblyTitle = "Vibeshine Uninstaller"
} else {
    $assemblyInfoPath = Join-Path $artifactDir "VibeshineInstaller.AssemblyInfo.cs"
    $assemblyTitle = "Vibeshine Installer"
}
$assemblyInfoContent = @(
    "using System.Reflection;",
    "[assembly: AssemblyTitle(""$assemblyTitle"")]",
    "[assembly: AssemblyProduct(""Vibeshine"")]",
    "[assembly: AssemblyCompany(""Nonary"")]",
    "[assembly: AssemblyVersion(""$assemblyVersion"")]",
    "[assembly: AssemblyFileVersion(""$assemblyVersion"")]",
    "[assembly: AssemblyInformationalVersion(""$informationalVersion"")]"
)
Set-Content -Path $assemblyInfoPath -Value $assemblyInfoContent -Encoding UTF8

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

if ($UninstallOnly) {
    Write-Host "[bootstrapper] Lightweight uninstaller build complete."
} else {
    Write-Host "[bootstrapper] Custom installer build complete."
}
