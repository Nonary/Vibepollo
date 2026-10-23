param([string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path)
$ErrorActionPreference = 'Stop'
function Assert-That([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message } }
$fixture = @'
function Get-WindowsDriver {
    param([switch]$Online, [switch]$All, $ErrorAction)
    if (!$Online -or $All) { throw 'Display inventory must exclude inbox driver enumeration.' }
    [pscustomobject]@{ OriginalFileName='C:\DriverStore\component.inf'; Driver='oem1.inf' }
    [pscustomobject]@{ OriginalFileName='C:\DriverStore\other.inf'; Driver='oem2.inf' }
}
function Get-CimInstance {
    param($ClassName, $ErrorAction)
    if ($ClassName -ne 'Win32_PnPSignedDriver') { throw 'Unexpected inventory provider.' }
    [pscustomobject]@{ DeviceID='ROOT\TEST\0000'; HardwareID='ROOT\TEST'; DriverVersion='1.2.0.11' }
    [pscustomobject]@{ DeviceID='PCI\OTHER'; HardwareID='PCI\OTHER'; DriverVersion='9.9.9.9' }
}
'@
foreach ($driver in @('sunshine','sudovda')) {
    $tokens = $null; $errors = $null
    $path = Join-Path $RepositoryRoot "src_assets/windows/drivers/$driver/install.ps1"
    $ast = [Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors)
    Assert-That (!$errors.Count) "$driver installer must parse"
    foreach ($name in @('Get-DriverInstallSafetyInventory','ConvertFrom-DriverInstallSafetyInventoryResult')) {
        $node = $ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name}.GetNewClosure(), $true)
        . ([scriptblock]::Create($node.Extent.Text))
    }
    # Run the production child-query body against read-only fake providers.
    # Its real exit statements execute in a child, never in this test host.
    function Invoke-DriverInstallSafetyQuery {
        param($ScriptText, $TimeoutSeconds)
        $script:observedTimeout = $TimeoutSeconds
        $command = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($fixture + "`n" + $ScriptText))
        $lines = & "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -EncodedCommand $command 2>&1
        if ($LASTEXITCODE -ne 0) { throw "Production $driver query failed: $lines" }
        return $lines -join "`n"
    }
    $inventory = Get-DriverInstallSafetyInventory -InfName 'component.inf' -HardwareId 'ROOT\TEST'
    Assert-That ($script:observedTimeout -eq 15) 'The inventory deadline must remain bounded'
    Assert-That ($inventory.Packages.Count -eq 1 -and $inventory.Packages[0].Driver -eq 'oem1.inf') "$driver must retain the matching Driver Store package"
    Assert-That ($inventory.Devices.Count -eq 1 -and $inventory.Devices[0].DriverVersion -eq '1.2.0.11') "$driver must retain the active matching device version"
    Write-Output "$driver production inventory query passed without inbox enumeration."
}
