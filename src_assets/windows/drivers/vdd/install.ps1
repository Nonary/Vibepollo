param(
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $PSCommandPath
$hardwareId = 'Root\MttVDD'
$classGuid = '{4D36E968-E325-11CE-BFC1-08002BE10318}'
$nefConc = Join-Path $scriptDir 'nefconc.exe'
$infPath = Join-Path $scriptDir 'MttVDD.inf'
$certPath = Join-Path $scriptDir 'MttVDD.cer'
$catPath = Join-Path $scriptDir 'mttvdd.cat'
$dllPath = Join-Path $scriptDir 'MttVDD.dll'
$sourceSettingsPath = Join-Path $scriptDir 'vdd_settings.xml'
$configRoot = Join-Path $env:ProgramData 'VirtualDisplayDriver'
$registryPath = 'HKLM:\SOFTWARE\MikeTheTech\VirtualDisplayDriver'
$temporaryDisplaysRegistryPath = Join-Path $registryPath 'TemporaryDisplays'

function Resolve-SystemToolPath {
    param([Parameter(Mandatory = $true)][string]$ToolName)

    $systemRoot = if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) { 'C:\Windows' } else { $env:SystemRoot }
    foreach ($candidate in @(
        (Join-Path $systemRoot "Sysnative\$ToolName"),
        (Join-Path $systemRoot "System32\$ToolName"),
        (Join-Path $systemRoot "SysWOW64\$ToolName")
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    return Join-Path $systemRoot "System32\$ToolName"
}

$pnputil = Resolve-SystemToolPath -ToolName 'pnputil.exe'

function Invoke-DriverProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [int]$TimeoutSeconds = 120,
        [int[]]$AllowedExitCodes = @(0, 3010)
    )

    $quotedArgs = foreach ($argument in $ArgumentList) {
        $arg = [string]$argument
        if ($arg -notmatch '[\s"]') {
            $arg
        } else {
            '"' + ($arg -replace '\\(?=")', '\' -replace '"', '\"') + '"'
        }
    }
    $process = Start-Process -FilePath $FilePath -ArgumentList ($quotedArgs -join ' ') -WorkingDirectory $scriptDir -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -notin $AllowedExitCodes) {
        throw "[VDD] $FilePath failed with exit code $($process.ExitCode)."
    }
}

function Install-VddDriverPackage {
    Write-Host '[VDD] Installing VDD driver package.'
    Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/add-driver', $infPath, '/install') -AllowedExitCodes @(0, 259, 3010)
}

function Configure-VddSettings {
    New-Item -ItemType Directory -Force -Path $configRoot | Out-Null

    if (Test-Path -LiteralPath $sourceSettingsPath -PathType Leaf) {
        $targetSettingsPath = Join-Path $configRoot 'vdd_settings.xml'
        if (-not (Test-Path -LiteralPath $targetSettingsPath -PathType Leaf)) {
            Copy-Item -LiteralPath $sourceSettingsPath -Destination $targetSettingsPath -Force
        }
    }

    New-Item -Path $registryPath -Force | Out-Null
    New-ItemProperty -Path $registryPath -Name 'VDDPATH' -Value $configRoot -PropertyType String -Force | Out-Null

    New-Item -Path $temporaryDisplaysRegistryPath -Force | Out-Null
    $registryAcl = Get-Acl -Path $temporaryDisplaysRegistryPath
    $registryRights =
        [System.Security.AccessControl.RegistryRights]::ReadKey -bor
        [System.Security.AccessControl.RegistryRights]::SetValue -bor
        [System.Security.AccessControl.RegistryRights]::CreateSubKey -bor
        [System.Security.AccessControl.RegistryRights]::EnumerateSubKeys -bor
        [System.Security.AccessControl.RegistryRights]::Notify
    $registryRule = [System.Security.AccessControl.RegistryAccessRule]::new(
        'NT AUTHORITY\LOCAL SERVICE',
        $registryRights,
        [System.Security.AccessControl.InheritanceFlags]::ContainerInherit,
        [System.Security.AccessControl.PropagationFlags]::None,
        [System.Security.AccessControl.AccessControlType]::Allow
    )
    $registryAcl.SetAccessRule($registryRule)
    Set-Acl -Path $temporaryDisplaysRegistryPath -AclObject $registryAcl
}

function Assert-Artifact {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "[VDD] Required driver artifact missing: $Path"
    }
    if ((Get-Item -LiteralPath $Path).Length -le 0) {
        throw "[VDD] Required driver artifact is empty: $Path"
    }
}

function Install-Certificate {
    param([Parameter(Mandatory = $true)][string]$StoreName)

    Assert-Artifact -Path $certPath
    $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new([System.IO.File]::ReadAllBytes($certPath))
    $store = [System.Security.Cryptography.X509Certificates.X509Store]::new($StoreName, 'LocalMachine')
    try {
        $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        $existing = $store.Certificates.Find([System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint, $cert.Thumbprint, $false)
        if ($existing.Count -eq 0) {
            $store.Add($cert)
            Write-Host "[VDD] Certificate installed into LocalMachine\$StoreName."
        } else {
            Write-Host "[VDD] Certificate already present in LocalMachine\$StoreName."
        }
    } finally {
        $store.Close()
    }
}

function Stop-VddHostProcesses {
    $hosts = @(Get-CimInstance Win32_Process -Filter "Name = 'WUDFHost.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like '*DeviceGroupId:MttVDDGroup*' })

    if ($hosts.Count -eq 0) {
        return
    }

    foreach ($hostProcess in $hosts) {
        Write-Host "[VDD] Stopping stale UMDF host process pid=$($hostProcess.ProcessId)."
        Stop-Process -Id $hostProcess.ProcessId -Force -ErrorAction SilentlyContinue
    }

    $deadline = (Get-Date).AddSeconds(10)
    do {
        $remaining = @(Get-CimInstance Win32_Process -Filter "Name = 'WUDFHost.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.CommandLine -like '*DeviceGroupId:MttVDDGroup*' })
        if ($remaining.Count -eq 0) {
            return
        }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)

    Write-Warning '[VDD] A MttVDD UMDF host process is still present after stop request.'
}

if ($Uninstall) {
    Write-Host '[VDD] Removing VDD device node.'
    Stop-VddHostProcesses
    if (Test-Path -LiteralPath $nefConc -PathType Leaf) {
        try {
            Invoke-DriverProcess -FilePath $nefConc -ArgumentList @('--remove-device-node', '--hardware-id', $hardwareId, '--class-guid', $classGuid)
        } catch {
            Write-Warning $_.Exception.Message
        }
    }
    Stop-VddHostProcesses
    Write-Host '[VDD] Uninstall complete.'
    exit 0
}

foreach ($artifact in @($infPath, $dllPath, $catPath, $certPath)) {
    Assert-Artifact -Path $artifact
}

Configure-VddSettings

Install-Certificate -StoreName 'Root'
Install-Certificate -StoreName 'TrustedPublisher'

Stop-VddHostProcesses

Install-VddDriverPackage

if (Test-Path -LiteralPath $nefConc -PathType Leaf) {
    Write-Host '[VDD] Recreating VDD device node so the current driver package and interface registration are applied.'
    try {
        Invoke-DriverProcess -FilePath $nefConc -ArgumentList @('--remove-device-node', '--hardware-id', $hardwareId, '--class-guid', $classGuid)
    } catch {
        Write-Warning $_.Exception.Message
    }
    Stop-VddHostProcesses
    Invoke-DriverProcess -FilePath $nefConc -ArgumentList @('--create-device-node', '--class-name', 'Display', '--class-guid', $classGuid, '--hardware-id', $hardwareId)
    Install-VddDriverPackage
    Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/scan-devices')
    Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/restart-device', 'ROOT\DISPLAY\0001')
} else {
    Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/scan-devices')
    Invoke-DriverProcess -FilePath $pnputil -ArgumentList @('/restart-device', 'ROOT\DISPLAY\0001')
}

Write-Host '[VDD] Driver install complete.'
