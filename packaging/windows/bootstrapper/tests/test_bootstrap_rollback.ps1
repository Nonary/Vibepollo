param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$sourceText = Get-Content -LiteralPath (Join-Path $repo 'packaging/windows/bootstrapper/VibeshineInstaller.cs') -Raw -Encoding UTF8
$namespace = if ($sourceText.Contains('namespace VibepolloInstaller')) { 'VibepolloInstaller' } else { 'VibeshineInstaller' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
# Only replace the named external boundaries, never the production rollback
# methods. The harness verifies byte-exact cache copy and ordering using scratch
# files; it cannot install products or change registry/services/drivers/trust.
$boundaries = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'rollback_boundary_methods.cs.in') -Raw -Encoding UTF8
$boundaryMethods = [regex]::Matches($boundaries, '(?ms)^    (?:private|public) static [^\r\n]+\b(?<name>\w+)\([^\r\n]*\) \{\r?\n.*?^    \}')
if ($boundaryMethods.Count -ne 17) { throw 'Expected sixteen shared and one optional downstream rollback test boundary' }
foreach ($boundary in $boundaryMethods) {
    $name = $boundary.Groups['name'].Value
    $parameters = if ($name -eq 'TryGetPayloadMsiInfo') { '\(string msiPath\)' } else { '\(' }
    $pattern = '(?ms)^    (?:private|public) static [^\r\n]+\b' + $name + $parameters + '.*?^    \}'
    $production = [regex]::Matches($sourceText, $pattern)
    if ($name -eq 'GetInstalledVibepolloProduct' -and $production.Count -eq 0) { continue }
    if ($production.Count -ne 1) { throw "Cannot locate single production boundary $name" }
    $sourceText = $sourceText.Remove($production[0].Index, $production[0].Length).Insert($production[0].Index, $boundary.Value)
}
$productionPath = Join-Path $OutputDirectory 'rollback-production.cs'
$harnessPath = Join-Path $OutputDirectory 'rollback-regression.cs'
[IO.File]::WriteAllText($productionPath, $sourceText, [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText($harnessPath, (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'rollback_regression.cs') -Raw -Encoding UTF8).Replace('PRODUCT_NAMESPACE', $namespace), [Text.UTF8Encoding]::new($false))
$framework = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319'
$references = @()
foreach ($reference in @('System.dll','System.Core.dll','System.Data.dll','System.Xml.dll','System.Xaml.dll','System.Windows.Forms.dll','WPF/WindowsBase.dll','WPF/PresentationCore.dll','WPF/PresentationFramework.dll')) { $references += '/reference:' + (Join-Path $framework $reference) }
$output = Join-Path $OutputDirectory 'rollback-regression.exe'
& (Join-Path $framework 'csc.exe') /nologo /target:exe /main:RollbackRegression "/out:$output" @references $productionPath $harnessPath
if ($LASTEXITCODE -ne 0) { throw 'Production rollback harness compilation failed' }
& $output
if ($LASTEXITCODE -ne 0) { throw 'Production rollback regression failed' }
