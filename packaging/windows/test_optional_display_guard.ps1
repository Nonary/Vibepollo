param([string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path)
$ErrorActionPreference = 'Stop'
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('optional-display-guard-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $scratch | Out-Null
$hostExe = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
try {
    foreach ($driver in @('sunshine', 'sudovda')) {
        $path = Join-Path $RepositoryRoot "src_assets/windows/drivers/$driver/install.ps1"
        $tokens = $null; $errors = $null
        $ast = [Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors)
        if ($errors.Count) { throw "Parse failed: $path" }
        # Execute only the real top-level read-only guard with a mocked decision.
        # Never execute installer functions, device queries or the installer body.
        $guard = @($ast.EndBlock.Statements | Where-Object {
            $_.Extent.Text.Contains('Test-SafeDriverInstall -InfPath')
        })
        if ($guard.Count -ne 1) { throw "Ambiguous guard: $driver" }
        foreach ($bestEffort in @($false, $true)) {
            foreach ($decision in @('reject-trust', 'reject-query', 'skip-equal', 'eligible')) {
                $decisionBody = switch ($decision) {
                    'reject-trust' { 'throw "VIRTUAL_DISPLAY_DRIVER_WARNING: untrusted catalog"' }
                    'reject-query' { 'throw "Unknown installed component version"' }
                    'skip-equal' { 'return $false' }
                    'eligible' { 'return $true' }
                }
                $probe = Join-Path $scratch "$driver-$bestEffort-$decision.ps1"
                $prefix = '$ErrorActionPreference="Stop"; $InstallerBestEffort=' + ('$' + $bestEffort.ToString().ToLowerInvariant()) + '; $Uninstall=$false; $ValidateOnly=$false; $infPath="mock.inf"; $catPath="mock.cat"; $hardwareId="ROOT\MOCK"' + "`n"
                $source = $prefix + 'function Test-SafeDriverInstall { param($InfPath,$CatalogPath,$HardwareId) ' + $decisionBody + " }`n" + $guard[0].Extent.Text + "`nWrite-Output 'GUARD_ELIGIBLE'; exit 0"
                [IO.File]::WriteAllText($probe, $source)
                $ErrorActionPreference = 'Continue'
                try {
                    $output = (& $hostExe -NoProfile -File $probe 2>&1 | Out-String)
                    $exitCode = $LASTEXITCODE
                } finally { $ErrorActionPreference = 'Stop' }
                $reject = $decision.StartsWith('reject-')
                if ($reject -and !$bestEffort) {
                    if ($exitCode -eq 0) { throw "Strict $driver accepted $decision" }
                } elseif ($exitCode -ne 0) { throw "Optional $driver failed $decision`: $output" }
                if ($output.Contains('GUARD_ELIGIBLE') -ne ($decision -eq 'eligible')) {
                    throw "Guard continuation mismatch: $driver/$bestEffort/$decision"
                }
                $normalizedOutput = $output -replace '\s+', ' '
                if ($reject -and $bestEffort -and (!$normalizedOutput.Contains('VIRTUAL_DISPLAY_DRIVER_WARNING') -or !$normalizedOutput.Contains('driver state unchanged'))) {
                    throw "Missing actionable warning: $driver/$decision"
                }
            }
        }
    }
    [xml]$wix = Get-Content (Join-Path $RepositoryRoot 'packaging/windows/wix/custom_actions.wxs') -Raw
    $ns = New-Object Xml.XmlNamespaceManager($wix.NameTable)
    $ns.AddNamespace('w', 'http://schemas.microsoft.com/wix/2006/wi')
    foreach ($name in @('Sudovda','VirtualDisplayDriver')) {
        $set = $wix.SelectSingleNode("//w:CustomAction[@Id='SetInstall$name']", $ns)
        $action = $wix.SelectSingleNode("//w:CustomAction[@Id='Install$name']", $ns)
        # Shine keeps SudoVDA post-guard installation failures fatal; Apollo
        # ignores them. Both must explicitly request a nonfatal guard skip.
        if (!$set.Value.Contains('-InstallerBestEffort') -or ($name -eq 'VirtualDisplayDriver' -and $action.Return -ne 'ignore')) { throw "MSI optional policy missing: $name" }
    }
    Write-Host '16 isolated guard cases and MSI wiring passed; no installer or driver action executed.'
} finally {
    foreach ($file in Get-ChildItem -LiteralPath $scratch -File) { Remove-Item -LiteralPath $file.FullName }
    Remove-Item -LiteralPath $scratch
}
