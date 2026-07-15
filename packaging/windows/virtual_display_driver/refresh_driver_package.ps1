param(
    [Parameter(Mandatory = $true)]
    [string]$LibVirtualDisplayDir,

    [Parameter(Mandatory = $true)]
    [string]$PackageDir,

    [switch]$Build,
    [switch]$ValidateOnly,
    [string]$BuildDir,
    [string]$PrebuiltPackageDir,
    [string]$PackageVersion,
    [string]$DriverVerDate,
    [string]$VsDevCmdPath,
    [string]$SigningThumbprint,
    [switch]$SkipSigning
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

function Resolve-PrebuiltPackageRoot {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ''
    }

    $resolved = Resolve-RequiredPath -Path $Path
    $directDriverDir = Join-Path $resolved 'driver'
    $directToolsDir = Join-Path $resolved 'tools'
    if ((Test-Path -LiteralPath $directDriverDir -PathType Container) -and
        (Test-Path -LiteralPath $directToolsDir -PathType Container)) {
        return $resolved
    }

    $driverDirs = @(Get-ChildItem -LiteralPath $resolved -Recurse -Directory -Filter 'driver' -ErrorAction SilentlyContinue)
    foreach ($driverDir in $driverDirs) {
        $candidateRoot = Split-Path -Parent $driverDir.FullName
        $toolsDir = Join-Path $candidateRoot 'tools'
        if (Test-Path -LiteralPath $toolsDir -PathType Container) {
            return $candidateRoot
        }
    }

    throw "[SunshineVirtualDisplay] Unable to locate prebuilt driver/tools package layout under $resolved"
}

function Resolve-PackageVersionFromGit {
    param([Parameter(Mandatory = $true)][string]$Path)

    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) {
        return ''
    }

    $describe = & $git.Source -C $Path describe --tags --long --match 'v[0-9]*' 2>$null
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($describe)) {
        return ''
    }

    if ($describe -match '^v?([0-9]+\.[0-9]+\.[0-9]+)(?:-([0-9]+)-g[0-9a-f]+)?(?:-.+)?$') {
        $baseVersion = $Matches[1]
        $commitsSinceTag = if ($Matches.Count -gt 2 -and $Matches[2]) { [int]$Matches[2] } else { 0 }
        $dirty = & $git.Source -C $Path status --porcelain 2>$null
        if ($LASTEXITCODE -eq 0 -and $dirty) {
            $commitsSinceTag++
        }
        if ($commitsSinceTag -gt 0) {
            return "$baseVersion.$commitsSinceTag"
        }
        return $baseVersion
    }

    return ''
}

function Resolve-PackageVersionFromPrebuiltRoot {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ''
    }

    $current = Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue
    while ($current) {
        if ($current.Name -match '^libvirtualdisplay-([0-9]+\.[0-9]+\.[0-9]+(?:\.[0-9]+)?)-windows-[^-]+$') {
            return $Matches[1]
        }
        $parent = Split-Path -Parent $current.FullName
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $current.FullName) {
            break
        }
        $current = Get-Item -LiteralPath $parent -ErrorAction SilentlyContinue
    }

    return ''
}

function Resolve-NextLocalDirtyPackageVersion {
    param(
        [Parameter(Mandatory = $true)][string]$LibRoot,
        [Parameter(Mandatory = $true)][string]$Version,
        [string]$ExistingInfPath,
        [switch]$Advance
    )

    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) {
        return $Version
    }

    $dirty = & $git.Source -C $LibRoot status --porcelain 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $dirty -or
        [string]::IsNullOrWhiteSpace($ExistingInfPath) -or
        -not (Test-Path -LiteralPath $ExistingInfPath -PathType Leaf)) {
        return $Version
    }

    $numericVersion = ($Version.TrimStart('v') -replace '[-+].*$', '')
    if ($numericVersion -match '^([0-9]+)\.([0-9]+)\.([0-9]+)(?:\.([0-9]+))?$') {
        $major = [int]$Matches[1]
        $minor = [int]$Matches[2]
        $patch = [int]$Matches[3]
        $revision = if ($Matches[4]) { [int]$Matches[4] } else { 0 }
    } else {
        return $Version
    }

    $existingInf = Get-Content -LiteralPath $ExistingInfPath -Raw
    if ($existingInf -notmatch '(?m)^\s*DriverVer\s*=\s*[0-9]{2}/[0-9]{2}/[0-9]{4}\s*,\s*([0-9]+)\.([0-9]+)\.([0-9]+)\.([0-9]+)\s*$') {
        return $Version
    }

    $existingMajor = [int]$Matches[1]
    $existingMinor = [int]$Matches[2]
    $existingPatch = [int]$Matches[3]
    $existingRevision = [int]$Matches[4]
    if ($existingMajor -ne $major -or $existingMinor -ne $minor -or $existingPatch -ne $patch -or
        $existingRevision -lt $revision) {
        return $Version
    }

    if (-not $Advance) {
        return "$existingMajor.$existingMinor.$existingPatch.$existingRevision"
    }

    if ($existingRevision -ge 65535) {
        throw "[SunshineVirtualDisplay] Local dirty DriverVer revision is exhausted for $major.$minor.$patch."
    }

    $nextVersion = "$major.$minor.$patch.$($existingRevision + 1)"
    Write-Host "[SunshineVirtualDisplay] Advancing local dirty package version from $Version to $nextVersion so Windows restages changed driver payloads."
    return $nextVersion
}

function Resolve-DriverVerDateFromGit {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ($DriverVerDate) {
        return $DriverVerDate
    }

    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git) {
        $date = & $git.Source -C $Path log -1 '--format=%cd' '--date=format:%m/%d/%Y' 2>$null
        if ($LASTEXITCODE -eq 0 -and $date -match '^[0-9][0-9]/[0-9][0-9]/[0-9][0-9][0-9][0-9]$') {
            return $date
        }
    }

    return (Get-Date).ToUniversalTime().ToString('MM/dd/yyyy', [Globalization.CultureInfo]::InvariantCulture)
}

function Resolve-PackageVersion {
    param(
        [Parameter(Mandatory = $true)][string]$LibRoot,
        [string]$PrebuiltRoot,
        [string]$ExistingInfPath,
        [switch]$AdvanceLocalDirtyVersion
    )

    if ($PackageVersion) {
        return $PackageVersion.TrimStart('v')
    }

    $fromPrebuilt = Resolve-PackageVersionFromPrebuiltRoot -Path $PrebuiltRoot
    if ($fromPrebuilt) {
        return $fromPrebuilt
    }

    $fromGit = Resolve-PackageVersionFromGit -Path $LibRoot
    if ($fromGit) {
        return Resolve-NextLocalDirtyPackageVersion -LibRoot $LibRoot -Version $fromGit -ExistingInfPath $ExistingInfPath -Advance:$AdvanceLocalDirtyVersion
    }

    return '0.0.0'
}

function ConvertTo-DriverVerVersion {
    param([Parameter(Mandatory = $true)][string]$Version)

    $numericVersion = ($Version.TrimStart('v') -replace '[-+].*$', '')
    if ($numericVersion -match '^([0-9]+)\.([0-9]+)\.([0-9]+)$') {
        return "$($Matches[1]).$($Matches[2]).$($Matches[3]).0"
    }
    if ($numericVersion -match '^([0-9]+)\.([0-9]+)\.([0-9]+)\.([0-9]+)$') {
        return $numericVersion
    }

    throw "[SunshineVirtualDisplay] Package version is not usable as a Windows DriverVer version: $Version"
}

function Assert-DriverVer {
    param(
        [Parameter(Mandatory = $true)][string]$InfPath,
        [Parameter(Mandatory = $true)][string]$ExpectedVersion
    )

    $infText = Get-Content -LiteralPath $InfPath -Raw
    if ($infText -notmatch '(?m)^\s*DriverVer\s*=\s*([0-9]{2}/[0-9]{2}/[0-9]{4})\s*,\s*([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)\s*$') {
        throw "[SunshineVirtualDisplay] INF is missing a valid DriverVer line: $InfPath"
    }

    if ($Matches[2] -ne $ExpectedVersion) {
        throw "[SunshineVirtualDisplay] INF DriverVer version $($Matches[2]) does not match expected driver version $ExpectedVersion."
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

    function Find-PrivateCertificateByThumbprint {
        param([Parameter(Mandatory = $true)][string]$Thumbprint)

        foreach ($storeLocation in @('CurrentUser', 'LocalMachine')) {
            $store = [System.Security.Cryptography.X509Certificates.X509Store]::new('My', $storeLocation)
            try {
                $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadOnly)
                $matches = $store.Certificates.Find([System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint, $Thumbprint, $false)
                foreach ($match in $matches) {
                    if ($match.HasPrivateKey) {
                        return [PSCustomObject]@{
                            Certificate = $match
                            Thumbprint = $Thumbprint
                            UseMachineStore = ($storeLocation -eq 'LocalMachine')
                        }
                    }
                }
            } finally {
                $store.Close()
            }
        }

        return $null
    }

    function Export-PackageCertificate {
        param(
            [Parameter(Mandatory = $true)][System.Security.Cryptography.X509Certificates.X509Certificate2]$Certificate,
            [Parameter(Mandatory = $true)][string]$Path
        )

        $directory = Split-Path -Parent $Path
        if ($directory -and -not (Test-Path -LiteralPath $directory -PathType Container)) {
            New-Item -ItemType Directory -Force -Path $directory | Out-Null
        }
        [System.IO.File]::WriteAllBytes($Path, $Certificate.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))
    }

    if ($SigningThumbprint) {
        $thumbprint = $SigningThumbprint
    } elseif (Test-Path -LiteralPath $CertificatePath -PathType Leaf) {
        $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new([System.IO.File]::ReadAllBytes($CertificatePath))
        $thumbprint = $cert.Thumbprint
    } else {
        $thumbprint = ''
    }

    if ($thumbprint) {
        $matchedCertificate = Find-PrivateCertificateByThumbprint -Thumbprint $thumbprint
        if ($matchedCertificate) {
            if ($SigningThumbprint -or -not $matchedCertificate.UseMachineStore) {
                return $matchedCertificate
            }
            Write-Warning "[SunshineVirtualDisplay] Packaged certificate $thumbprint is only available in LocalMachine\My; selecting a user-store signing certificate when possible."
        }

        if ($SigningThumbprint) {
            throw "[SunshineVirtualDisplay] Signing certificate $thumbprint was not found with a private key."
        }

        Write-Warning "[SunshineVirtualDisplay] Packaged certificate $thumbprint has no matching private key; selecting a usable signing certificate."
    }

    $candidateSubjects = @(
        'CN=Sunshine Virtual Display Test',
        'CN=Virtual Display Driver Local Signing'
    )
    $fallbacks = [System.Collections.Generic.List[object]]::new()
    $store = [System.Security.Cryptography.X509Certificates.X509Store]::new('My', 'CurrentUser')
    try {
        $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadOnly)
        foreach ($match in $store.Certificates) {
            if (-not $match.HasPrivateKey -or $match.NotAfter -le (Get-Date)) {
                continue
            }
            if ($candidateSubjects -contains $match.Subject) {
                $fallbacks.Add([PSCustomObject]@{
                    Certificate = $match
                    Thumbprint = $match.Thumbprint
                    UseMachineStore = $false
                })
            }
        }
    } finally {
        $store.Close()
    }

    $fallback = $fallbacks |
        Sort-Object -Property @{Expression = { $_.UseMachineStore }; Descending = $false}, @{Expression = { $_.Certificate.NotAfter }; Descending = $true} |
        Select-Object -First 1
    if ($fallback) {
        Export-PackageCertificate -Certificate $fallback.Certificate -Path $CertificatePath
        return $fallback
    }

    $newSelfSignedCertificate = Get-Command New-SelfSignedCertificate -ErrorAction SilentlyContinue
    if ($newSelfSignedCertificate) {
        $generated = New-SelfSignedCertificate `
            -Type CodeSigningCert `
            -Subject 'CN=Sunshine Virtual Display Test' `
            -KeyUsage DigitalSignature `
            -KeyExportPolicy Exportable `
            -CertStoreLocation 'Cert:\CurrentUser\My' `
            -NotAfter (Get-Date).AddYears(5)
        Export-PackageCertificate -Certificate $generated -Path $CertificatePath
        return [PSCustomObject]@{
            Certificate = $generated
            Thumbprint = $generated.Thumbprint
            UseMachineStore = $false
        }
    }

    return [PSCustomObject]@{
        Thumbprint = ''
        UseMachineStore = $false
    }
}

$libRoot = Resolve-RequiredPath -Path $LibVirtualDisplayDir
$packageRoot = Resolve-RequiredPath -Path $PackageDir
$driverSourceInf = Join-Path $libRoot 'src\driver\windows_driver\SunshineVirtualDisplayDriver.inf'
Resolve-RequiredPath -Path $driverSourceInf -Leaf | Out-Null
$prebuiltPackageRoot = Resolve-PrebuiltPackageRoot -Path $PrebuiltPackageDir
$resolvedPackageVersion = Resolve-PackageVersion -LibRoot $libRoot -PrebuiltRoot $prebuiltPackageRoot -ExistingInfPath (Join-Path $packageRoot 'SunshineVirtualDisplayDriver.inf') -AdvanceLocalDirtyVersion:$Build
$resolvedDriverVersion = ConvertTo-DriverVerVersion -Version $resolvedPackageVersion
$resolvedDriverDate = Resolve-DriverVerDateFromGit -Path $libRoot

if (-not $BuildDir) {
    $BuildDir = Join-Path $libRoot 'build-driver'
}

if ($Build -and -not $prebuiltPackageRoot) {
    $vsDevCmd = Resolve-VsDevCmd
    $cmake = Resolve-Tool -Name 'cmake.exe'
    $ninja = Resolve-Tool -Name 'ninja.exe'
    Write-Host "[SunshineVirtualDisplay] Building local driver package version $resolvedPackageVersion (DriverVer $resolvedDriverDate,$resolvedDriverVersion)."
    $buildCommand = "`"$vsDevCmd`" -arch=x64 -host_arch=x64 && `"$cmake`" -S `"$libRoot`" -B `"$BuildDir`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DBUILD_TESTS=OFF -DBUILD_SUNSHINE_VIRTUAL_DISPLAY_DRIVER=ON -DBUILD_VIRTUALDISPLAY_PROBE=ON -DBUILD_VIRTUALDISPLAY_VULKAN_LAYER=ON -DCMAKE_BUILD_TYPE=Release -DLIBVIRTUALDISPLAY_PACKAGE_VERSION=`"$resolvedPackageVersion`" -DLIBVIRTUALDISPLAY_DRIVER_VERSION=`"$resolvedDriverVersion`" -DLIBVIRTUALDISPLAY_DRIVER_DATE=`"$resolvedDriverDate`" && `"$cmake`" --build `"$BuildDir`" --target SunshineVirtualDisplayDriverPackageFiles virtualdisplay_probe vk_layer_sunshine_hdr -j 10"
    Invoke-Cmd -Command $buildCommand
}

$driverBuildDir = Join-Path $BuildDir 'src\driver\windows_driver'
$driverBuildDll = Join-Path $driverBuildDir 'SunshineVirtualDisplayDriver.dll'
$driverBuildInf = Join-Path $driverBuildDir 'SunshineVirtualDisplayDriver.inf'
$probeBuildExe = Join-Path $BuildDir 'src\driver\virtualdisplay_probe.exe'
$vulkanLayerBuildDll = Join-Path $BuildDir 'src\driver\VkLayer_sunshine_hdr.dll'
$vulkanLayerBuildJson = Join-Path $BuildDir 'src\driver\VkLayer_sunshine_hdr.json'

$driverPackageDir = @(Get-ChildItem -LiteralPath $driverBuildDir -Recurse -Directory -Filter 'driver-package' -ErrorAction SilentlyContinue |
    Sort-Object -Property FullName |
    Select-Object -First 1)
if ($driverPackageDir) {
    $packagedDriverDll = Join-Path $driverPackageDir.FullName 'SunshineVirtualDisplayDriver.dll'
    $packagedDriverInf = Join-Path $driverPackageDir.FullName 'SunshineVirtualDisplayDriver.inf'
    if ((Test-Path -LiteralPath $packagedDriverDll -PathType Leaf) -and
        (Test-Path -LiteralPath $packagedDriverInf -PathType Leaf)) {
        $driverBuildDll = $packagedDriverDll
        $driverBuildInf = $packagedDriverInf
    }
}

$packageDll = Join-Path $packageRoot 'SunshineVirtualDisplayDriver.dll'
$packageInf = Join-Path $packageRoot 'SunshineVirtualDisplayDriver.inf'
$packageCat = Join-Path $packageRoot 'SunshineVirtualDisplayDriver.cat'
$packageCer = Join-Path $packageRoot 'SunshineVirtualDisplayDriver.cer'
$packageProbe = Join-Path $packageRoot 'virtualdisplay_probe.exe'
$packageInstaller = Join-Path $packageRoot 'install.ps1'
$packageVulkanLayerDir = Join-Path $packageRoot 'vulkan-layer'
$packageVulkanLayerDll = Join-Path $packageVulkanLayerDir 'VkLayer_sunshine_hdr.dll'
$packageVulkanLayerJson = Join-Path $packageVulkanLayerDir 'VkLayer_sunshine_hdr.json'
$expectedPackageDll = ''
$expectedPackageInf = ''
$expectedPackageCat = ''
$expectedPackageCer = ''
$expectedPackageProbe = ''
$expectedPackageVulkanLayerDll = ''
$expectedPackageVulkanLayerJson = ''

if ($prebuiltPackageRoot) {
    $expectedPackageDll = Join-Path $prebuiltPackageRoot 'driver\SunshineVirtualDisplayDriver.dll'
    $expectedPackageInf = Join-Path $prebuiltPackageRoot 'driver\SunshineVirtualDisplayDriver.inf'
    $expectedPackageCat = Join-Path $prebuiltPackageRoot 'driver\SunshineVirtualDisplayDriver.cat'
    $expectedPackageCer = Join-Path $prebuiltPackageRoot 'driver\SunshineVirtualDisplayDriver.cer'
    $expectedPackageProbe = Join-Path $prebuiltPackageRoot 'tools\virtualdisplay_probe.exe'
    $expectedPackageVulkanLayerDll = Join-Path $prebuiltPackageRoot 'vulkan-layer\VkLayer_sunshine_hdr.dll'
    $expectedPackageVulkanLayerJson = Join-Path $prebuiltPackageRoot 'vulkan-layer\VkLayer_sunshine_hdr.json'
}

if ($Build) {
    New-Item -ItemType Directory -Path $packageVulkanLayerDir -Force | Out-Null
    if ($prebuiltPackageRoot) {
        Write-Host "[SunshineVirtualDisplay] Refreshing staged driver assets from prebuilt package: $prebuiltPackageRoot"
        Assert-File -Path $expectedPackageDll
        Assert-File -Path $expectedPackageInf
        Assert-File -Path $expectedPackageCat
        Assert-File -Path $expectedPackageProbe
        Assert-File -Path $expectedPackageVulkanLayerDll
        Assert-File -Path $expectedPackageVulkanLayerJson
        Copy-Item -Force -LiteralPath $expectedPackageDll -Destination $packageDll
        Copy-Item -Force -LiteralPath $expectedPackageInf -Destination $packageInf
        Copy-Item -Force -LiteralPath $expectedPackageCat -Destination $packageCat
        if (Test-Path -LiteralPath $expectedPackageCer -PathType Leaf) {
            Copy-Item -Force -LiteralPath $expectedPackageCer -Destination $packageCer
        } elseif (Test-Path -LiteralPath $packageCer -PathType Leaf) {
            Remove-Item -Force -LiteralPath $packageCer
            $expectedPackageCer = ''
        }
        Copy-Item -Force -LiteralPath $expectedPackageProbe -Destination $packageProbe
        Copy-Item -Force -LiteralPath $expectedPackageVulkanLayerDll -Destination $packageVulkanLayerDll
        Copy-Item -Force -LiteralPath $expectedPackageVulkanLayerJson -Destination $packageVulkanLayerJson
    } else {
        Assert-File -Path $driverBuildDll
        Assert-File -Path $driverBuildInf
        Assert-File -Path $probeBuildExe
        Assert-File -Path $vulkanLayerBuildDll
        Assert-File -Path $vulkanLayerBuildJson
        Copy-Item -Force -LiteralPath $driverBuildDll -Destination $packageDll
        Copy-Item -Force -LiteralPath $driverBuildInf -Destination $packageInf
        Copy-Item -Force -LiteralPath $probeBuildExe -Destination $packageProbe
        Copy-Item -Force -LiteralPath $vulkanLayerBuildDll -Destination $packageVulkanLayerDll
        Copy-Item -Force -LiteralPath $vulkanLayerBuildJson -Destination $packageVulkanLayerJson
        $expectedPackageDll = $driverBuildDll
        $expectedPackageInf = $driverBuildInf
        $expectedPackageProbe = $probeBuildExe
        $expectedPackageVulkanLayerDll = $vulkanLayerBuildDll
        $expectedPackageVulkanLayerJson = $vulkanLayerBuildJson

        $inf2Cat = Resolve-Tool -Name 'Inf2Cat.exe'
        & $inf2Cat /driver:$packageRoot /os:10_X64,10_RS5_X64,10_GE_X64,Server10_X64
        if ($LASTEXITCODE -ne 0) {
            throw "[SunshineVirtualDisplay] Inf2Cat failed with exit code $LASTEXITCODE"
        }

        if ($SkipSigning) {
            Write-Warning '[SunshineVirtualDisplay] Skipping local driver catalog signing; generated catalog is unsigned.'
        } else {
            $signingCert = Find-SigningCertificate -CertificatePath $packageCer
            if ($signingCert.Thumbprint) {
                $signtool = Resolve-Tool -Name 'signtool.exe'
                $signArgs = @('sign', '/fd', 'SHA256')
                if ($signingCert.UseMachineStore) {
                    $signArgs += '/sm'
                }
                $signArgs += @('/sha1', $signingCert.Thumbprint, $packageCat)
                Write-Host "[SunshineVirtualDisplay] Signing driver catalog with certificate $($signingCert.Thumbprint)."
                & $signtool @signArgs
                if ($LASTEXITCODE -ne 0) {
                    throw "[SunshineVirtualDisplay] signtool sign failed with exit code $LASTEXITCODE"
                }
                $signature = Get-AuthenticodeSignature -LiteralPath $packageCat
                if (-not $signature.SignerCertificate) {
                    throw "[SunshineVirtualDisplay] signed catalog has no signer certificate"
                }
            } else {
                Write-Warning '[SunshineVirtualDisplay] No signing certificate was provided; generated catalog is unsigned.'
            }
        }
    }
}

foreach ($artifact in @(
    $packageInstaller,
    (Join-Path $packageRoot 'nefconc.exe'),
    $packageDll,
    $packageInf,
    $packageCat,
    $packageProbe,
    $packageVulkanLayerDll,
    $packageVulkanLayerJson
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
    'HKR,,Security,,"D:P(A;;GA;;;SY)(A;;GA;;;S-1-5-80-2333729190-1599198784-3320592948-2337414441-3098439965)"',
    'HKR,,"ConfigVersion",0x00010001,1',
    'UmdfExtensions=IddCx0102'
)) {
    if ($infText -notlike "*$required*") {
        throw "[SunshineVirtualDisplay] INF is missing expected content: $required"
    }
}
if (-not $prebuiltPackageRoot -or $PackageVersion) {
    Assert-DriverVer -InfPath $packageInf -ExpectedVersion $resolvedDriverVersion
} else {
    try {
        Assert-DriverVer -InfPath $packageInf -ExpectedVersion $resolvedDriverVersion
    } catch {
        Write-Warning $_.Exception.Message
    }
}

if ($expectedPackageDll) {
    Assert-SameFile -Expected $expectedPackageDll -Actual $packageDll
}
if ($expectedPackageInf) {
    Assert-SameFile -Expected $expectedPackageInf -Actual $packageInf
}
if ($expectedPackageCat) {
    Assert-SameFile -Expected $expectedPackageCat -Actual $packageCat
}
if ($expectedPackageCer) {
    Assert-SameFile -Expected $expectedPackageCer -Actual $packageCer
}
if ($expectedPackageProbe) {
    Assert-SameFile -Expected $expectedPackageProbe -Actual $packageProbe
}
if ($expectedPackageVulkanLayerDll) {
    Assert-SameFile -Expected $expectedPackageVulkanLayerDll -Actual $packageVulkanLayerDll
}
if ($expectedPackageVulkanLayerJson) {
    Assert-SameFile -Expected $expectedPackageVulkanLayerJson -Actual $packageVulkanLayerJson
}

$validateArgs = @('-NoLogo', '-NonInteractive', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $packageInstaller, '-ValidateOnly')
if ($SkipSigning) {
    $validateArgs += '-AllowUnsignedCatalogForValidation'
}
& powershell.exe @validateArgs
if ($LASTEXITCODE -ne 0) {
    throw "[SunshineVirtualDisplay] Driver installer validation failed with exit code $LASTEXITCODE"
}

if ($ValidateOnly) {
    Write-Host '[SunshineVirtualDisplay] Driver package assets validated.'
} else {
    Write-Host '[SunshineVirtualDisplay] Driver package assets refreshed and validated.'
}
