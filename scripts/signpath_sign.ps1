#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputArtifactPath,

    [string]$OutputArtifactPath = "",
    [string]$ApiToken = $env:SIGNPATH_API_TOKEN,
    [string]$OrganizationId = $(if ([string]::IsNullOrWhiteSpace($env:SIGNPATH_ORGANIZATION_ID)) { "1ba0e884-7ab4-43e6-aa84-9b2c7e3fba15" } else { $env:SIGNPATH_ORGANIZATION_ID }),
    [string]$ProjectSlug = $(if ([string]::IsNullOrWhiteSpace($env:SIGNPATH_PROJECT_SLUG)) { "Vibepollo" } else { $env:SIGNPATH_PROJECT_SLUG }),
    [string]$SigningPolicySlug = $(if ([string]::IsNullOrWhiteSpace($env:SIGNPATH_SIGNING_POLICY_SLUG)) { "test-signing" } else { $env:SIGNPATH_SIGNING_POLICY_SLUG }),
    [string]$ArtifactConfigurationSlug = "",
    [string]$PeArtifactConfigurationSlug = $env:SIGNPATH_PE_ARTIFACT_CONFIGURATION_SLUG,
    [string]$MsiArtifactConfigurationSlug = $(if ([string]::IsNullOrWhiteSpace($env:SIGNPATH_MSI_ARTIFACT_CONFIGURATION_SLUG)) { "msi-file" } else { $env:SIGNPATH_MSI_ARTIFACT_CONFIGURATION_SLUG }),
    [string]$Description = "",
    [int]$WaitForCompletionTimeoutInSeconds = 1800,
    [switch]$InstallModuleIfMissing,
    [switch]$SkipIfMissingToken
)

$ErrorActionPreference = "Stop"

function Resolve-PathStrict([string]$Path) {
    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    return $resolved.ProviderPath
}

function Get-FullPath([string]$Path) {
    return [System.IO.Path]::GetFullPath($Path)
}

function Ensure-SignPathCommand {
    if (Get-Command Submit-SigningRequest -ErrorAction SilentlyContinue) {
        return
    }

    if (-not $InstallModuleIfMissing) {
        throw "Submit-SigningRequest was not found. Install the SignPath PowerShell module, or rerun with -InstallModuleIfMissing."
    }

    Write-Host "[signpath] Submit-SigningRequest not found; installing SignPath PowerShell module for CurrentUser..."
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    if (-not (Get-PackageProvider -Name NuGet -ErrorAction SilentlyContinue)) {
        Install-PackageProvider -Name NuGet -Scope CurrentUser -Force | Out-Null
    }
    Install-Module -Name SignPath -Scope CurrentUser -Repository PSGallery -Force -AllowClobber -MinimumVersion 4.0.0 -MaximumVersion 4.999.999
    Import-Module SignPath -ErrorAction Stop

    if (-not (Get-Command Submit-SigningRequest -ErrorAction SilentlyContinue)) {
        throw "Submit-SigningRequest is still unavailable after installing the SignPath PowerShell module."
    }
}

function Resolve-ArtifactConfigurationSlug([string]$Path) {
    if (-not [string]::IsNullOrWhiteSpace($ArtifactConfigurationSlug)) {
        return $ArtifactConfigurationSlug
    }

    $extension = [System.IO.Path]::GetExtension($Path).ToLowerInvariant()
    switch ($extension) {
        { $_ -in @(".msi", ".msm", ".msp") } {
            return $MsiArtifactConfigurationSlug
        }
        { $_ -in @(".exe", ".dll", ".sys", ".ocx", ".cpl", ".drv", ".efi", ".mui", ".scr") } {
            return $PeArtifactConfigurationSlug
        }
        default {
            return ""
        }
    }
}

if ([string]::IsNullOrWhiteSpace($ApiToken)) {
    if ($SkipIfMissingToken) {
        Write-Warning "[signpath] SIGNPATH_API_TOKEN is not set; leaving artifact unsigned: $InputArtifactPath"
        return
    }

    throw "SignPath API token is required. Pass -ApiToken or set SIGNPATH_API_TOKEN."
}

$inputPath = Resolve-PathStrict $InputArtifactPath
if ([string]::IsNullOrWhiteSpace($OutputArtifactPath)) {
    $OutputArtifactPath = $inputPath
}

$outputPath = Get-FullPath $OutputArtifactPath
$outputDir = Split-Path -Parent $outputPath
if (-not [string]::IsNullOrWhiteSpace($outputDir) -and -not (Test-Path -LiteralPath $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

$samePath = [System.StringComparer]::OrdinalIgnoreCase.Equals($inputPath, $outputPath)
$signingOutputPath = $outputPath
if ($samePath) {
    $leaf = [System.IO.Path]::GetFileNameWithoutExtension($outputPath)
    $extension = [System.IO.Path]::GetExtension($outputPath)
    $signingOutputPath = Join-Path $outputDir (".signpath-$leaf-$([Guid]::NewGuid().ToString('N'))$extension")
}

Ensure-SignPathCommand

$resolvedArtifactConfigurationSlug = Resolve-ArtifactConfigurationSlug -Path $inputPath

$requestArgs = @{
    InputArtifactPath = $inputPath
    ApiToken = $ApiToken
    OrganizationId = $OrganizationId
    ProjectSlug = $ProjectSlug
    SigningPolicySlug = $SigningPolicySlug
    OutputArtifactPath = $signingOutputPath
    WaitForCompletion = $true
    Force = $true
}

if ($WaitForCompletionTimeoutInSeconds -gt 0) {
    $requestArgs.WaitForCompletionTimeoutInSeconds = $WaitForCompletionTimeoutInSeconds
}

if (-not [string]::IsNullOrWhiteSpace($resolvedArtifactConfigurationSlug)) {
    $requestArgs.ArtifactConfigurationSlug = $resolvedArtifactConfigurationSlug
}

if (-not [string]::IsNullOrWhiteSpace($Description)) {
    $requestArgs.Description = $Description
}

Write-Warning "[signpath] This runner-local Submit-SigningRequest path is NON-ORIGIN-VERIFIED (test/dev only). Release artifacts are signed via SignPath GitHub origin verification in CI; see docs/signpath/."

$inputExtension = [System.IO.Path]::GetExtension($inputPath).ToLowerInvariant()
if ([string]::IsNullOrWhiteSpace($resolvedArtifactConfigurationSlug) -and $inputExtension -eq ".exe") {
    Write-Warning "[signpath] No PE artifact-configuration slug resolved for $inputPath; relying on the SignPath project default. Set -PeArtifactConfigurationSlug or SIGNPATH_PE_ARTIFACT_CONFIGURATION_SLUG to pin one."
}

Write-Host "[signpath] Submitting signing request..."
Write-Host "[signpath] Input:  $inputPath"
Write-Host "[signpath] Output: $outputPath"
Write-Host "[signpath] Project: $ProjectSlug / $SigningPolicySlug"
if (-not [string]::IsNullOrWhiteSpace($resolvedArtifactConfigurationSlug)) {
    Write-Host "[signpath] Artifact configuration: $resolvedArtifactConfigurationSlug"
} else {
    Write-Host "[signpath] Artifact configuration: project default"
}

try {
    Submit-SigningRequest @requestArgs

    if ($samePath) {
        Move-Item -LiteralPath $signingOutputPath -Destination $outputPath -Force
    }
} finally {
    if ($samePath -and (Test-Path -LiteralPath $signingOutputPath)) {
        Remove-Item -LiteralPath $signingOutputPath -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "[signpath] Signed artifact written to: $outputPath"
