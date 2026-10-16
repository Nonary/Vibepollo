# Immutable upstream package contract shared by the build and elevated installer.
# Kernel files are copied unchanged from the signed release; this script never
# enables test signing or imports a publisher certificate.
$UsbIpVersion = '0.9.8.1'
$UsbIpReleaseTag = 'v.0.9.8.1'
$UsbIpInstallerName = 'USBip-0.9.8.1-x64.exe'
$UsbIpInstallerUrl = 'https://github.com/vadimgrn/usbip-win2/releases/download/v.0.9.8.1/USBip-0.9.8.1-x64.exe'
$UsbIpInstallerHash = '38cad6d4432b52d5bb9409d9ad03b72fdffc4ada4cd3a48fbeca1a2752a8518a'
$UsbIpFiles = @('usbip2_ude.inf', 'usbip2_ude.sys', 'usbip2_ude.cat',
                'usbip2_filter.inf', 'usbip2_filter.sys', 'usbip2_filter.cat')
$UsbIpUdeHashes = @{
    'usbip2_ude.inf' = '1ae9e8dc497929de1a6be1291ae1dca6a19e7fd6c0351e8c266f65dd91f15073'
    'usbip2_ude.sys' = 'abd4fc43dce40e027e4acc884cbfe92433e026206f6565c5ee5284a39441169a'
    'usbip2_ude.cat' = '84dd6cc5985857adf1514a9ee202210ff3ee1384c9be40fd49d6810e23e25f3a'
}

function Assert-UsbIpMicrosoftCatalog([string]$Path) {
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -notmatch '(^|,\s*)CN=Microsoft Windows Hardware Compatibility Publisher(,|$)') {
        throw "USB/IP catalog must have a valid Microsoft hardware publisher signature: $Path"
    }
}

function Assert-UsbIpPackage([string]$Directory) {
    $lockPath = Join-Path $Directory 'release-lock.json'
    $lock = Get-Content -LiteralPath $lockPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($lock.version -ne $UsbIpVersion -or $lock.tag -ne $UsbIpReleaseTag -or
        $lock.architecture -ne 'x64' -or $lock.installer_sha256 -ne $UsbIpInstallerHash) {
        throw 'USB/IP package does not match the pinned Microsoft-signed release.'
    }
    foreach ($name in $UsbIpFiles) {
        $path = Join-Path $Directory $name
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing USB/IP artifact: $name" }
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        if ($hash -ne $lock.files.$name -or
            ($UsbIpUdeHashes.ContainsKey($name) -and $hash -ne $UsbIpUdeHashes[$name])) {
            throw "USB/IP artifact hash mismatch: $name"
        }
    }
    Assert-UsbIpMicrosoftCatalog (Join-Path $Directory 'usbip2_ude.cat')
    Assert-UsbIpMicrosoftCatalog (Join-Path $Directory 'usbip2_filter.cat')
}
