param(
    [Parameter(Mandatory = $true)]
    [string]$LibVirtualDisplayDir,

    [Parameter(Mandatory = $true)]
    [string]$PackageDir,

    [switch]$Build,
    [switch]$ValidateOnly,
    [string]$BuildDir,
    [string]$VsDevCmdPath,
    [string]$SigningThumbprint
)

$ErrorActionPreference = 'Stop'

function Resolve-RequiredPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [switch]$Leaf
    )

    if ($Leaf) {
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
            throw "[SunshineVirtualDisplay] Required file missing: $Path"
        }
    } elseif (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "[SunshineVirtualDisplay] Required directory missing: $Path"
    }

    return (Resolve-Path -LiteralPath $Path).Path
}

function Resolve-Tool {
    param([Parameter(Mandatory = $true)][string]$Name)

    $isWindowsSdkTool = $Name -in @('Inf2Cat.exe', 'signtool.exe')
    $candidates = [System.Collections.Generic.List[string]]::new()
    if (-not $isWindowsSdkTool) {
        foreach ($candidate in @(
            "D:\Software\MSYS2\ucrt64\bin\$Name",
            "D:\Software\MSYS2\usr\bin\$Name",
            "C:\Program Files\CMake\bin\$Name"
        )) {
            $candidates.Add($candidate)
        }
    }

    foreach ($root in @(
        'D:\Software\WinSDK\bin',
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    )) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) {
            continue
        }

        $versions = Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue | Sort-Object -Property Name -Descending
        foreach ($version in $versions) {
            foreach ($arch in @('x64', 'x86')) {
                $candidates.Add((Join-Path $version.FullName "$arch\$Name"))
            }
        }
    }

    $candidates.Add("D:\Software\WinSDK\App Certification Kit\$Name")
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    if ($isWindowsSdkTool) {
        throw "[SunshineVirtualDisplay] Unable to locate Windows SDK $Name"
    }

    $fromPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($fromPath) {
        return $fromPath.Source
    }

    throw "[SunshineVirtualDisplay] Unable to locate $Name"
}

function Resolve-VsDevCmd {
    if ($VsDevCmdPath) {
        return Resolve-RequiredPath -Path $VsDevCmdPath -Leaf
    }

    $candidates = [System.Collections.Generic.List[string]]::new()
    foreach ($candidate in @(
        'D:\Software\Visual Studio\Common7\Tools\VsDevCmd.bat',
        'D:\Software\BuildTools\Common7\Tools\VsDevCmd.bat',
        'E:\Software\VS2022BuildTools\Common7\Tools\VsDevCmd.bat',
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
    )) {
        $candidates.Add($candidate)
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $installationPaths = @(& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath 2>$null)
        foreach ($installationPath in $installationPaths) {
            if (-not [string]::IsNullOrWhiteSpace($installationPath)) {
                $candidates.Add((Join-Path $installationPath 'Common7\Tools\VsDevCmd.bat'))
            }
        }
    }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw '[SunshineVirtualDisplay] Unable to locate VsDevCmd.bat. Pass -VsDevCmdPath to refresh the driver package.'
}

function Assert-File {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "[SunshineVirtualDisplay] Required package artifact missing: $Path"
    }

    if ((Get-Item -LiteralPath $Path).Length -le 0) {
        throw "[SunshineVirtualDisplay] Required package artifact is empty: $Path"
    }
}

function Assert-SameFile {
    param(
        [Parameter(Mandatory = $true)][string]$Expected,
        [Parameter(Mandatory = $true)][string]$Actual
    )

    $expectedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Expected).Hash
    $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Actual).Hash
    if ($expectedHash -ne $actualHash) {
        throw "[SunshineVirtualDisplay] Package artifact is stale: $Actual does not match $Expected"
    }
}

function Invoke-Cmd {
    param([Parameter(Mandatory = $true)][string]$Command)

    & cmd.exe /s /c $Command
    if ($LASTEXITCODE -ne 0) {
        throw "[SunshineVirtualDisplay] Command failed with exit code $LASTEXITCODE`: $Command"
    }
}

function Find-SigningCertificate {
    param([string]$CertificatePath)

    if ($SigningThumbprint) {
        $thumbprint = $SigningThumbprint
    } elseif (Test-Path -LiteralPath $CertificatePath -PathType Leaf) {
        $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new([System.IO.File]::ReadAllBytes($CertificatePath))
        $thumbprint = $cert.Thumbprint
    } else {
        return [PSCustomObject]@{
            Thumbprint = ''
            UseMachineStore = $false
        }
    }

    foreach ($storeLocation in @('CurrentUser', 'LocalMachine')) {
        $store = [System.Security.Cryptography.X509Certificates.X509Store]::new('My', $storeLocation)
        try {
            $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadOnly)
            $matches = $store.Certificates.Find([System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint, $thumbprint, $false)
            foreach ($match in $matches) {
                if ($match.HasPrivateKey) {
                    return [PSCustomObject]@{
                        Thumbprint = $thumbprint
                        UseMachineStore = ($storeLocation -eq 'LocalMachine')
                    }
                }
            }
        } finally {
            $store.Close()
        }
    }

    return [PSCustomObject]@{
        Thumbprint = $thumbprint
        UseMachineStore = $false
    }
}

$libRoot = Resolve-RequiredPath -Path $LibVirtualDisplayDir
$packageRoot = Resolve-RequiredPath -Path $PackageDir
$driverSourceInf = Join-Path $libRoot 'src\driver\windows_driver\SunshineVirtualDisplayDriver.inf'
Resolve-RequiredPath -Path $driverSourceInf -Leaf | Out-Null

if (-not $BuildDir) {
    $BuildDir = Join-Path $libRoot 'build-driver'
}

if ($Build) {
    $vsDevCmd = Resolve-VsDevCmd
    $cmake = Resolve-Tool -Name 'cmake.exe'
    $ninja = Resolve-Tool -Name 'ninja.exe'
    $buildCommand = "`"$vsDevCmd`" -arch=x64 -host_arch=x64 && `"$cmake`" -S `"$libRoot`" -B `"$BuildDir`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DBUILD_TESTS=OFF -DBUILD_SUNSHINE_VIRTUAL_DISPLAY_DRIVER=ON -DBUILD_VIRTUALDISPLAY_PROBE=ON -DCMAKE_BUILD_TYPE=Release && `"$cmake`" --build `"$BuildDir`" --target SunshineVirtualDisplayDriver virtualdisplay_probe -j 10"
    Invoke-Cmd -Command $buildCommand
}

$driverBuildDir = Join-Path $BuildDir 'src\driver\windows_driver'
$driverBuildDll = Join-Path $driverBuildDir 'SunshineVirtualDisplayDriver.dll'
$driverBuildInf = Join-Path $driverBuildDir 'SunshineVirtualDisplayDriver.inf'
$probeBuildExe = Join-Path $BuildDir 'src\driver\virtualdisplay_probe.exe'

$packageDll = Join-Path $packageRoot 'SunshineVirtualDisplayDriver.dll'
$packageInf = Join-Path $packageRoot 'SunshineVirtualDisplayDriver.inf'
$packageCat = Join-Path $packageRoot 'SunshineVirtualDisplayDriver.cat'
$packageCer = Join-Path $packageRoot 'SunshineVirtualDisplayDriver.cer'
$packageProbe = Join-Path $packageRoot 'virtualdisplay_probe.exe'
$packageInstaller = Join-Path $packageRoot 'install.ps1'

if ($Build) {
    Assert-File -Path $driverBuildDll
    Assert-File -Path $driverBuildInf
    Assert-File -Path $probeBuildExe
    Copy-Item -Force -LiteralPath $driverBuildDll -Destination $packageDll
    Copy-Item -Force -LiteralPath $driverSourceInf -Destination $packageInf
    Copy-Item -Force -LiteralPath $probeBuildExe -Destination $packageProbe

    $inf2Cat = Resolve-Tool -Name 'Inf2Cat.exe'
    & $inf2Cat /driver:$packageRoot /os:10_X64,10_RS5_X64,10_GE_X64,Server10_X64
    if ($LASTEXITCODE -ne 0) {
        throw "[SunshineVirtualDisplay] Inf2Cat failed with exit code $LASTEXITCODE"
    }

    $signingCert = Find-SigningCertificate -CertificatePath $packageCer
    if ($signingCert.Thumbprint) {
        $signtool = Resolve-Tool -Name 'signtool.exe'
        $signArgs = @('sign', '/fd', 'SHA256')
        if ($signingCert.UseMachineStore) {
            $signArgs += '/sm'
        }
        $signArgs += @('/sha1', $signingCert.Thumbprint, $packageCat)
        & $signtool @signArgs
        if ($LASTEXITCODE -ne 0) {
            throw "[SunshineVirtualDisplay] signtool sign failed with exit code $LASTEXITCODE"
        }
        & $signtool verify /pa $packageCat
        if ($LASTEXITCODE -ne 0) {
            throw "[SunshineVirtualDisplay] signtool verify failed with exit code $LASTEXITCODE"
        }
    } else {
        Write-Warning '[SunshineVirtualDisplay] No signing certificate was provided; generated catalog is unsigned.'
    }
}

foreach ($artifact in @(
    $packageInstaller,
    (Join-Path $packageRoot 'nefconc.exe'),
    $packageDll,
    $packageInf,
    $packageCat,
    $packageProbe
)) {
    Assert-File -Path $artifact
}

if (Test-Path -LiteralPath $packageCer -PathType Leaf) {
    Assert-File -Path $packageCer
}

$infText = Get-Content -LiteralPath $packageInf -Raw
foreach ($required in @(
    'Root\SunshineVirtualDisplay',
    'SunshineVirtualDisplayDriver.dll',
    'CatalogFile=SunshineVirtualDisplayDriver.cat',
    'AddInterface={5f894d6c-3a69-48a2-86ef-e4c671932d63},,ControlInterface',
    '[ControlInterface_AddReg]',
    'HKR,,Security,,"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)"',
    'HKR,,"ConfigVersion",0x00010001,1',
    'UmdfExtensions=IddCx0102'
)) {
    if ($infText -notlike "*$required*") {
        throw "[SunshineVirtualDisplay] INF is missing expected content: $required"
    }
}

if (Test-Path -LiteralPath $driverBuildDll -PathType Leaf) {
    Assert-SameFile -Expected $driverBuildDll -Actual $packageDll
}
if (Test-Path -LiteralPath $driverSourceInf -PathType Leaf) {
    Assert-SameFile -Expected $driverSourceInf -Actual $packageInf
}
if (Test-Path -LiteralPath $probeBuildExe -PathType Leaf) {
    Assert-SameFile -Expected $probeBuildExe -Actual $packageProbe
}

& powershell.exe -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass -File $packageInstaller -ValidateOnly
if ($LASTEXITCODE -ne 0) {
    throw "[SunshineVirtualDisplay] Driver installer validation failed with exit code $LASTEXITCODE"
}

if ($ValidateOnly) {
    Write-Host '[SunshineVirtualDisplay] Driver package assets validated.'
} else {
    Write-Host '[SunshineVirtualDisplay] Driver package assets refreshed and validated.'
}
