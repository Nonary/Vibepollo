param([string]$RepositoryRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)))
$ErrorActionPreference = 'Stop'

function Assert-Condition([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Expect-Failure([scriptblock]$Action) {
    $failed = $false
    try { & $Action } catch { $failed = $true }
    Assert-Condition $failed 'Expected package validation to fail.'
}

# Exercise the installer's real functions without running its administrative
# entry point. Only the registry boundary is replaced; no machine keys change.
$installer = Join-Path $RepositoryRoot 'src_assets/windows/drivers/sunshine/install.ps1'
$tokens = $null
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($installer, [ref]$tokens, [ref]$errors)
Assert-Condition ($errors.Count -eq 0) 'Installer PowerShell syntax is invalid.'
$functions = @('Assert-Artifact', 'Assert-VulkanLayerPackage', 'Unregister-VulkanLayer')
foreach ($node in $ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true)) {
    if ($node.Name -in $functions) { Invoke-Expression $node.Extent.Text }
}

class RegistryFixture {
    [hashtable]$Values = @{}
    [string[]] GetValueNames() { return @($this.Values.Keys) }
    [void] DeleteValue([string]$Name, [bool]$ThrowIfMissing) { $this.Values.Remove($Name) }
    [void] Dispose() {}
}
$registry = @{
    Registry64 = [RegistryFixture]::new()
    Registry32 = [RegistryFixture]::new()
}
function Open-LocalMachineRegistryKey {
    param($View, $SubKey)
    Assert-Condition ($SubKey -eq $vulkanImplicitLayersSubKey) 'Wrong registry key.'
    return $registry[$View.ToString()]
}
function Write-PeFixture([string]$Path, [uint16]$Machine) {
    $bytes = [byte[]]::new(128)
    $bytes[0] = 0x4d; $bytes[1] = 0x5a
    [BitConverter]::GetBytes([int]64).CopyTo($bytes, 0x3c)
    [BitConverter]::GetBytes([uint32]0x00004550).CopyTo($bytes, 64)
    [BitConverter]::GetBytes($Machine).CopyTo($bytes, 68)
    [IO.File]::WriteAllBytes($Path, $bytes)
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('vulkan-layer-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tempRoot | Out-Null
try {
    $vulkanImplicitLayersSubKey = 'SOFTWARE\Khronos\Vulkan\ImplicitLayers'
    $vulkanLayers = @(
        @{ View = [Microsoft.Win32.RegistryView]::Registry64; Manifest = (Join-Path $tempRoot 'VkLayer_sunshine_hdr.json'); Dll = (Join-Path $tempRoot 'VkLayer_sunshine_hdr.dll'); Machine = 0x8664 },
        @{ View = [Microsoft.Win32.RegistryView]::Registry32; Manifest = (Join-Path $tempRoot 'VkLayer_sunshine_hdr_x86.json'); Dll = (Join-Path $tempRoot 'VkLayer_sunshine_hdr_x86.dll'); Machine = 0x014c }
    )
    $sourceManifest = Join-Path $RepositoryRoot 'src_assets/windows/drivers/sunshine/vulkan-layer/VkLayer_sunshine_hdr.json'
    foreach ($layer in $vulkanLayers) {
        Write-PeFixture $layer.Dll $layer.Machine
        $manifest = Get-Content -LiteralPath $sourceManifest -Raw | ConvertFrom-Json
        $manifest.layer.library_path = '.\' + [IO.Path]::GetFileName($layer.Dll)
        $manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $layer.Manifest
    }
    $old64 = Join-Path $tempRoot 'old/VkLayer_sunshine_hdr.json'
    $old32 = Join-Path $tempRoot 'old/VkLayer_sunshine_hdr_x86.json'
    foreach ($key in $registry.Values) {
        $key.Values['ReShade.json'] = 0
        $key.Values['SteamOverlay.json'] = 1
    }
    Assert-VulkanLayerPackage

    # Reject an x64 DLL substituted for x86, and reject a manifest that points
    # to the other architecture.
    Write-PeFixture $vulkanLayers[1].Dll 0x8664
    Expect-Failure { Assert-VulkanLayerPackage }
    Write-PeFixture $vulkanLayers[1].Dll 0x014c
    Copy-Item -LiteralPath $vulkanLayers[0].Manifest -Destination $vulkanLayers[1].Manifest -Force
    Expect-Failure { Assert-VulkanLayerPackage }
    Remove-Item -LiteralPath $vulkanLayers[1].Dll
    Expect-Failure { Assert-VulkanLayerPackage }

    # An incomplete pair still needs removal when the option is disabled.
    foreach ($onlyView in @('Registry64', 'Registry32')) {
        $registry[$onlyView].Values[$old64] = 0
        $registry[$onlyView].Values[$old32] = 0
        Unregister-VulkanLayer
        Unregister-VulkanLayer
        foreach ($key in $registry.Values) {
            Assert-Condition ($key.Values.Count -eq 2) 'Unregistration left an owned value or removed another layer.'
            Assert-Condition ($key.Values['ReShade.json'] -eq 0 -and $key.Values['SteamOverlay.json'] -eq 1) 'Third-party values changed.'
        }
    }
    Write-Host 'Dual-view Vulkan HDR cleanup and payload validation passed.'
} finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force
}
