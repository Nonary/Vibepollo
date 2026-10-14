$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$sourcePath = Join-Path $repo 'packaging/windows/bootstrapper/VibeshineInstaller.cs'
$framework = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319'
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('driver-selections-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($scratch) | Out-Null
try {
    $harness = Join-Path $scratch 'regression.cs'
    (Get-Content (Join-Path $PSScriptRoot 'driver_selections_regression.cs') -Raw).Replace('PRODUCT_NAMESPACE', 'VibepolloInstaller') | Set-Content $harness
    $output = Join-Path $scratch 'regression.exe'
    $compileArgs = @('/nologo', '/target:exe', '/main:DriverSelectionsRegression', "/out:$output")
    foreach ($reference in @('System.dll', 'System.Core.dll', 'System.Data.dll', 'System.Xml.dll', 'System.Xaml.dll', 'System.Windows.Forms.dll', 'WPF/WindowsBase.dll', 'WPF/PresentationCore.dll', 'WPF/PresentationFramework.dll')) {
        $compileArgs += '/reference:' + (Join-Path $framework $reference)
    }
    $compileArgs += @($sourcePath, $harness)
    & (Join-Path $framework csc.exe) @compileArgs
    if ($LASTEXITCODE -ne 0) { throw 'Production driver-selection compilation failed.' }
    & $output
    if ($LASTEXITCODE -ne 0) { throw 'Driver-selection regression failed.' }
    & (Join-Path $PSScriptRoot 'test_gamepad_bundle_metadata.ps1') -RegressionExe $output -OutputDirectory $scratch
    [xml]$patch = Get-Content (Join-Path $repo 'packaging/windows/wix/patch_custom_actions.wxs') -Raw
    foreach ($id in @('SetInstallSudovda', 'InstallSudovda', 'SetInstallVirtualDisplayDriver', 'InstallVirtualDisplayDriver')) {
        $action = $patch.SelectSingleNode("//InstallExecuteSequence/Custom[@Action='$id']")
        if ($null -eq $action -or $action.InnerText -notmatch 'INSTALL_DISPLAY_DRIVERS\s*=\s*"1"') { throw "Display opt-out not enforced by $id" }
    }
    foreach ($id in @('RestoreSavedDisplayDriverSelection', 'DefaultDisplayDriverSelection', 'RestoreSavedGamepadDriverSelection', 'DefaultGamepadDriverSelection')) {
        $action = $patch.SelectSingleNode("//InstallExecuteSequence/Custom[@Action='$id']")
        if ($null -eq $action -or -not $action.InnerText.Contains('=""')) { throw "Explicit MSI caller selection can be overridden by $id" }
    }
    foreach ($id in @('INSTALL_DISPLAY_DRIVERS', 'INSTALL_VIRTUAL_GAMEPAD_DRIVER')) {
        $property = $patch.SelectSingleNode("//Property[@Id='$id']")
        if ($property.HasAttribute('Value') -or $property.Secure -ne 'yes') { throw "MSI persistence/caller precedence lost for $id" }
    }
    $feature = $patch.SelectSingleNode('//Feature[@Id="InstallerDriverSelections"]')
    if ($null -eq $feature -or $feature.SelectNodes('ComponentRef').Count -ne 1 -or $feature.ComponentRef.Id -ne 'InstallerDriverSelectionPreferences') { throw 'Preference maintenance must target only its own registry feature.' }
    foreach ($id in @('EnsureDriverSelectionsRepair', 'ExtendDriverSelectionsRepair', 'DefaultDriverSelectionsRepairMode')) {
        $action = $patch.SelectSingleNode("//InstallExecuteSequence/Custom[@Action='$id']")
        if ($null -eq $action -or -not $action.InnerText.Contains('Installed AND NOT REMOVE')) { throw "Same-product maintenance/uninstall guard missing: $id" }
    }
    $ensure = $patch.SelectSingleNode('//InstallExecuteSequence/Custom[@Action="EnsureDriverSelectionsRepair"]')
    $mode = $patch.SelectSingleNode('//InstallExecuteSequence/Custom[@Action="DefaultDriverSelectionsRepairMode"]')
    if (-not $ensure.InnerText.Contains('REINSTALL=""') -or -not $mode.InnerText.Contains('REINSTALLMODE=""')) { throw 'Explicit caller repair flags must be preserved.' }
    $source = Get-Content $sourcePath -Raw
    foreach ($binding in @('IsChecked = _driverSelections.DisplayEnabled', 'IsChecked = _driverSelections.GamepadEnabled', '_driverSelections.SelectDisplay(false)', '_driverSelections.SelectGamepad(false)', 'BuildDriverSelectionArguments(arguments, installVirtualDisplayDriver, installVirtualGamepadDriver)')) {
        if (-not $source.Contains($binding)) { throw "Actual UI/MSI selection binding lost: $binding" }
    }
    $runtime = Get-Content (Join-Path $repo 'src_assets/windows/drivers/vhf-gamepad/install.ps1') -Raw
    if (-not $runtime.Contains("7F1868BF86698A8674F40E686B02C8C702BD3B88")) { throw 'Reusable beta identity pin missing.' }
    Write-Host 'Production UI wiring, MSI opt-out gates and explicit caller precedence passed.'
} finally {
    # Only remove this newly created, known temporary test directory.
    $resolved = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $scratch).Path)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolved) -notmatch '^driver-selections-[0-9a-f]{32}$') { throw 'Invalid scratch cleanup target.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
