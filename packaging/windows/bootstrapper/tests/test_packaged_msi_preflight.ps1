# Exercise the production preview against the packaged MSI. Never run an install
# sequence, remove a product, create the selected directory, or stop a service.
param(
    [Parameter(Mandatory=$true)][string]$MsiPath,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$MsiPath = (Resolve-Path -LiteralPath $MsiPath).ProviderPath
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$framework = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319'
$references = @()
foreach ($reference in @('System.dll','System.Core.dll','System.Data.dll','System.Xml.dll','System.Xaml.dll','System.Windows.Forms.dll','WPF/WindowsBase.dll','WPF/PresentationCore.dll','WPF/PresentationFramework.dll')) {
    $references += '/reference:' + (Join-Path $framework $reference)
}
$assemblyPath = Join-Path $OutputDirectory 'preflight-production.dll'
& (Join-Path $framework 'csc.exe') /nologo /target:library "/out:$assemblyPath" @references (Join-Path $repo 'packaging/windows/bootstrapper/VibeshineInstaller.cs')
if ($LASTEXITCODE -ne 0) { throw 'Production preflight compilation failed' }
$assembly = [Reflection.Assembly]::LoadFrom($assemblyPath)
$runner = $assembly.GetType('VibepolloInstaller.InstallerRunner', $true)
$method = $runner.GetMethod('PreflightIncomingMsi', [Reflection.BindingFlags]'NonPublic,Static')
if ($null -eq $method) { throw 'Production MSI preflight is missing' }
$leaf = 'VibepolloPreflight-' + [Guid]::NewGuid().ToString('N')
$validRoot = Join-Path $env:ProgramFiles $leaf
$invalidRoot = Join-Path $OutputDirectory $leaf
foreach ($case in @(@{ Root=$validRoot; Accepted=$true }, @{ Root=$invalidRoot; Accepted=$false })) {
    if (Test-Path -LiteralPath $case.Root) { throw 'Preflight test destination must not exist' }
    # Join-Path output can carry a PSObject wrapper. A typed array unwraps
    # every argument before it reaches the reflection binder (PowerShell 5.1).
    [object[]]$invokeArguments = @([string]$MsiPath, [string]$case.Root, [string]'packaged_msi_preflight', $null)
    $result = $method.Invoke($null, $invokeArguments)
    if ($result.Succeeded -ne $case.Accepted) {
        $detail = if ($null -eq $result) { 'unexpected acceptance' } else { $result.Message }
        throw "Packaged MSI preflight returned the wrong result: $detail"
    }
    if (Test-Path -LiteralPath $case.Root) { throw 'Preflight created the install directory' }
}
Write-Host 'Production preflight accepted Program Files and rejected an outside directory without installing.'
