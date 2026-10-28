# Read-only evidence collector and advisory planner. There is deliberately no
# apply mode: this report must never be used as authorization to mutate trust.
#Requires -Version 5.1
[CmdletBinding()]
param()

function ConvertTo-LegacyTrustThumbprint {
    param([string]$Value)
    if ($Value -notmatch '\A[0-9a-fA-F]{40}\z') { throw 'Missing or invalid certificate thumbprint.' }
    return $Value.ToUpperInvariant()
}

function Get-LegacyTrustCatalogIdentity {
    param([Parameter(Mandatory = $true)][string]$Path)
    Add-Type -AssemblyName System.Security -ErrorAction Stop
    $bytes = [IO.File]::ReadAllBytes($Path)
    $cms = New-Object Security.Cryptography.Pkcs.SignedCms
    $cms.Decode($bytes)
    if ($cms.ContentInfo.ContentType.Value -ne '1.3.6.1.4.1.311.10.1') { throw 'Not a Windows catalog CTL.' }
    $signers = @(); $certificates = @()
    $pending = @([pscustomobject]@{ Cms=$cms; Depth=0 })
    while ($pending.Count -gt 0) {
        $entry = $pending[0]
        $pending = @($pending | Select-Object -Skip 1)
        if ($entry.Depth -gt 8 -or $entry.Cms.SignerInfos.Count -eq 0) { throw 'Missing or unsupported nested catalog signature.' }
        # Verify signature bytes without changing trust or requiring this signer
        # to remain trusted. Expired legacy identities must still be preserved.
        $entry.Cms.CheckSignature($true)
        foreach ($cert in $entry.Cms.Certificates) {
            $certificates += ConvertTo-LegacyTrustThumbprint $cert.Thumbprint
        }
        foreach ($signer in $entry.Cms.SignerInfos) {
            if (!$signer.Certificate) { throw 'Catalog signer certificate unavailable.' }
            $signers += ConvertTo-LegacyTrustThumbprint $signer.Certificate.Thumbprint
            $certificates += ConvertTo-LegacyTrustThumbprint $signer.Certificate.Thumbprint
            foreach ($attribute in $signer.UnsignedAttributes) {
                if ($attribute.Oid.Value -eq '1.3.6.1.4.1.311.2.4.1') {
                    foreach ($value in $attribute.Values) {
                        $nested = New-Object Security.Cryptography.Pkcs.SignedCms
                        $nested.Decode($value.RawData)
                        $pending += [pscustomobject]@{ Cms=$nested; Depth=($entry.Depth + 1) }
                    }
                }
            }
        }
    }
    $hash = [Security.Cryptography.SHA256]::Create()
    try { $digest = [BitConverter]::ToString($hash.ComputeHash($bytes)).Replace('-', '') } finally { $hash.Dispose() }
    return [pscustomobject]@{
        Path=[IO.Path]::GetFullPath($Path); Sha256=$digest
        SignerThumbprints=@($signers | Sort-Object -Unique)
        # Including every embedded certificate deliberately over-preserves trust.
        CertificateThumbprints=@($certificates | Sort-Object -Unique)
    }
}

function Get-LegacyTrustNativeEnvironment {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT -or ![Environment]::Is64BitProcess) {
        throw 'A native 64-bit Windows PowerShell process is required.'
    }
    $root = [IO.Path]::GetFullPath($env:SystemRoot)
    return [pscustomobject]@{
        WindowsRoot=$root
        CatalogRoots=@((Join-Path $root 'System32/DriverStore/FileRepository'), (Join-Path $root 'System32/CatRoot'))
    }
}

function Get-LegacyTrustNativePackages { @(Get-WindowsDriver -Online -All -ErrorAction Stop) }
function Get-LegacyTrustNativeDevices { @(Get-CimInstance -ClassName Win32_PnPSignedDriver -ErrorAction Stop) }

function Get-LegacyTrustNativeCatalogFiles {
    param([string[]]$Roots)
    $files = @()
    foreach ($root in $Roots) {
        $directory = Get-Item -LiteralPath $root -Force -ErrorAction Stop
        if (!$directory.PSIsContainer -or ($directory.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Unsafe catalog inventory root: $root"
        }
        $entries = @(Get-ChildItem -LiteralPath $root -Recurse -Force -ErrorAction Stop)
        if (@($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count -gt 0) {
            throw "Reparse point in catalog inventory: $root"
        }
        $files += @($entries | Where-Object { !$_.PSIsContainer -and $_.Extension -ieq '.cat' } | ForEach-Object { $_.FullName })
    }
    if ($files.Count -eq 0) { throw 'Catalog inventory unexpectedly empty.' }
    return @($files | Sort-Object -Unique)
}

function Get-LegacyTrustNativeTrust {
    foreach ($name in @('Root', 'TrustedPublisher')) {
        $store = [Security.Cryptography.X509Certificates.X509Store]::new($name, 'LocalMachine')
        try {
            $store.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadOnly -bor [Security.Cryptography.X509Certificates.OpenFlags]::OpenExistingOnly)
            foreach ($cert in $store.Certificates) {
                [pscustomobject]@{ Store=$name; Thumbprint=(ConvertTo-LegacyTrustThumbprint $cert.Thumbprint) }
            }
        } finally { $store.Close() }
    }
}

function Get-LegacyTrustNativePendingReboot {
    $machine = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
    try {
        foreach ($name in @('SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending',
                            'SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired')) {
            $key = $machine.OpenSubKey($name, $false)
            if ($key) { $key.Dispose(); return $true }
        }
        $key = $machine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager', $false)
        if (!$key) { throw 'Cannot read pending driver/file replacement state.' }
        try {
            foreach ($name in @('PendingFileRenameOperations','PendingFileRenameOperations2')) {
                if (@($key.GetValue($name, $null) | Where-Object { ![string]::IsNullOrEmpty([string]$_) }).Count -gt 0) { return $true }
            }
        } finally { $key.Dispose() }
    } finally { $machine.Dispose() }
    return $false
}

function Get-LegacyTrustDeclaredCatalogs {
    param([string]$InfPath)
    $inVersion = $false; $seenVersion = $false; $catalogs = @(); $keys = @()
    foreach ($line in (Get-Content -LiteralPath $InfPath -ErrorAction Stop)) {
        # The inventory accepts only simple literal catalog names. Any INF
        # syntax we cannot resolve exactly makes the entire plan preserve trust.
        $text = ($line -split ';', 2)[0].Trim()
        if ($text -match '^\[(.+)\]$') {
            $inVersion = $Matches[1] -ieq 'Version'
            if ($inVersion -and $seenVersion) { throw 'Ambiguous INF Version section.' }
            if ($inVersion) { $seenVersion = $true }
            continue
        }
        if ($inVersion -and $text -match '^CatalogFile([^=]*)=(.*)$') {
            $key = ('CatalogFile' + $Matches[1].Trim()).ToUpperInvariant()
            $name = $Matches[2].Trim().Trim('"')
            if ($key -notmatch '^CATALOGFILE(\.NT(X86|AMD64|ARM|ARM64|IA64)?)?$' -or $key -in $keys -or
                $name -notmatch '\A[a-zA-Z0-9_. -]+\.cat\z' -or $name -in @('.', '..')) {
                throw 'Unknown or ambiguous INF catalog declaration.'
            }
            $keys += $key
            $catalogs += Join-Path (Split-Path -Parent $InfPath) $name
        }
    }
    if (!$seenVersion) { throw 'Missing INF Version section.' }
    return @($catalogs | Sort-Object -Unique)
}

function Get-LegacyDriverTrustSnapshot {
    $issues = @(); $packages = @(); $active = @(); $catalogs = @(); $trust = @(); $environment = $null
    try {
        $environment = Get-LegacyTrustNativeEnvironment
        if (Get-LegacyTrustNativePendingReboot) { throw 'Pending reboot or file replacement makes driver inventory uncertain.' }
        $rawPackages = @(Get-LegacyTrustNativePackages)
        $rawDevices = @(Get-LegacyTrustNativeDevices)
        $catalogFiles = @(Get-LegacyTrustNativeCatalogFiles -Roots $environment.CatalogRoots)
        foreach ($path in $catalogFiles) { $catalogs += Get-LegacyTrustCatalogIdentity -Path $path }
        foreach ($package in $rawPackages) {
            $published = [IO.Path]::GetFileName([string]$package.Driver)
            $path = [string]$package.OriginalFileName
            if ($published -notmatch '\A[^\\/:]+\.inf\z' -or $package.Inbox -isnot [bool] -or ![IO.Path]::IsPathRooted($path) -or
                @($packages | Where-Object { $_.PublishedInf -ieq $published }).Count -ne 0) {
                throw 'Unknown or duplicate Driver Store package identity.'
            }
            $path = [IO.Path]::GetFullPath($path)
            $safeParents = @((Join-Path $environment.WindowsRoot 'System32/DriverStore/FileRepository'), (Join-Path $environment.WindowsRoot 'INF'))
            if (@($safeParents | Where-Object { $path.StartsWith(($_ + [IO.Path]::DirectorySeparatorChar), [StringComparison]::OrdinalIgnoreCase) }).Count -eq 0) {
                throw 'Driver Store INF location cannot be resolved safely.'
            }
            if ((Get-Item -LiteralPath $path -Force -ErrorAction Stop).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse point INF cannot qualify for cleanup.' }
            $declared = @(Get-LegacyTrustDeclaredCatalogs -InfPath $path)
            # Inbox INFs can be catalogless: their catalogs live in CatRoot.
            # Every catalog there was already included in the conservative scan.
            if (!$package.Inbox -and $declared.Count -eq 0) { throw 'Third-party INF has no resolved catalog.' }
            foreach ($catalog in $declared) {
                if (@($catalogs | Where-Object { $_.Path -ieq $catalog }).Count -ne 1) { throw 'A declared Driver Store catalog is missing from inventory.' }
            }
            $packages += [pscustomobject]@{
                PublishedInf=$published.ToLowerInvariant(); OriginalInf=$path; Inbox=$package.Inbox
                InfSha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256 -ErrorAction Stop).Hash
                Catalogs=$declared
            }
        }
        if ($packages.Count -eq 0) { throw 'Driver Store inventory unexpectedly empty.' }
        foreach ($device in $rawDevices) {
            $inf = [string]$device.InfName
            if ($inf -notmatch '\A[^\\/:]+\.inf\z' -or [string]::IsNullOrWhiteSpace([string]$device.DeviceID) -or
                @($packages | Where-Object { $_.PublishedInf -ieq $inf }).Count -ne 1) {
                throw 'An active INF binding is missing or unresolved in Driver Store inventory.'
            }
            $active += [pscustomobject]@{ DeviceId=[string]$device.DeviceID; PublishedInf=$inf.ToLowerInvariant() }
        }
        $trust = @(Get-LegacyTrustNativeTrust | Sort-Object Store, Thumbprint -Unique)
    } catch { $issues += $_.Exception.Message }
    return [pscustomobject]@{
        Schema=1; Complete=($issues.Count -eq 0); Issues=$issues
        Packages=@($packages | Sort-Object PublishedInf); Active=@($active | Sort-Object DeviceId, PublishedInf)
        Catalogs=@($catalogs | Sort-Object Path); Trust=$trust
    }
}

function New-LegacyDriverTrustCleanupPlan {
    param([Parameter(Mandatory = $true)]$Before, [Parameter(Mandatory = $true)]$After)
    $pins = @('37E100B2B382866FA7CE04EE33FE53DF53AEDC6D', '3C918FC73525AD8B1521B6DB26B71F694277CC49')
    $complete = $true; $reason = 'NoCatalogDependency'; $decisions = @(); $proposals = @()
    try {
        foreach ($snapshot in @($Before, $After)) {
            foreach ($field in @('Schema','Complete','Issues','Packages','Active','Catalogs','Trust')) {
                if ($field -notin $snapshot.PSObject.Properties.Name) { throw 'Incomplete snapshot shape.' }
            }
            if ($snapshot.Schema -ne 1 -or $snapshot.Complete -isnot [bool] -or !$snapshot.Complete -or
                @($snapshot.Issues).Count -ne 0 -or @($snapshot.Packages).Count -eq 0 -or @($snapshot.Catalogs).Count -eq 0) { throw 'Incomplete inventory.' }
            foreach ($package in $snapshot.Packages) {
                foreach ($field in @('PublishedInf','OriginalInf','Inbox','InfSha256','Catalogs')) {
                    if ($field -notin $package.PSObject.Properties.Name) { throw 'Incomplete package evidence.' }
                }
                if ($package.PublishedInf -notmatch '\A[^\\/:]+\.inf\z' -or $package.Inbox -isnot [bool] -or
                    ![IO.Path]::IsPathRooted([string]$package.OriginalInf) -or $package.InfSha256 -notmatch '\A[0-9A-Fa-f]{64}\z' -or
                    @($snapshot.Packages | Where-Object { $_.PublishedInf -ieq $package.PublishedInf }).Count -ne 1 -or
                    (!$package.Inbox -and @($package.Catalogs).Count -eq 0)) { throw 'Unknown package evidence.' }
                foreach ($path in $package.Catalogs) {
                    if (@($snapshot.Catalogs | Where-Object { $_.Path -ieq $path }).Count -ne 1) { throw 'Unresolved declared catalog.' }
                }
            }
            foreach ($device in $snapshot.Active) {
                if (!$device.DeviceId -or !$device.PublishedInf -or
                    @($snapshot.Packages | Where-Object { $_.PublishedInf -ieq $device.PublishedInf }).Count -ne 1) { throw 'Unknown active binding.' }
            }
            foreach ($catalog in $snapshot.Catalogs) {
                if (!$catalog.Path -or ![IO.Path]::IsPathRooted([string]$catalog.Path) -or
                    @($snapshot.Catalogs | Where-Object { $_.Path -ieq $catalog.Path }).Count -ne 1 -or $catalog.Sha256 -notmatch '\A[0-9A-Fa-f]{64}\z' -or
                    @($catalog.SignerThumbprints).Count -eq 0 -or @($catalog.CertificateThumbprints).Count -eq 0) { throw 'Unverified catalog identity.' }
                foreach ($pin in @($catalog.SignerThumbprints) + @($catalog.CertificateThumbprints)) { $null = ConvertTo-LegacyTrustThumbprint $pin }
                foreach ($pin in $catalog.SignerThumbprints) { if ($pin -notin $catalog.CertificateThumbprints) { throw 'Incomplete catalog certificates.' } }
            }
            foreach ($entry in $snapshot.Trust) {
                if ($entry.Store -notin @('Root','TrustedPublisher')) { throw 'Unknown trust store.' }
                $null = ConvertTo-LegacyTrustThumbprint $entry.Thumbprint
            }
        }
        if (($Before | ConvertTo-Json -Depth 12 -Compress) -cne ($After | ConvertTo-Json -Depth 12 -Compress)) {
            $complete = $false; $reason = 'InventoryChanged'
        }
    } catch { $complete = $false; $reason = 'InventoryIncomplete' }
    foreach ($pin in $pins) {
        $dependencies = @($After.Catalogs | Where-Object { $pin -in $_.CertificateThumbprints } | ForEach-Object { $_.Path })
        $stores = @($After.Trust | Where-Object { $_.Thumbprint -ieq $pin -and $_.Store -in @('Root','TrustedPublisher') } | ForEach-Object { $_.Store } | Sort-Object -Unique)
        $action = 'Preserve'; $why = $reason
        if ($complete) {
            if ($dependencies.Count -gt 0) { $why = 'CatalogDependency' }
            elseif ($stores.Count -eq 0) { $action = 'Absent'; $why = 'NotTrusted' }
            else {
                $action = 'ReviewRemoval'
                foreach ($store in $stores) { $proposals += [pscustomobject]@{ Store="LocalMachine\$store"; Thumbprint=$pin } }
            }
        }
        $decisions += [pscustomobject]@{ Thumbprint=$pin; Decision=$action; Reason=$why; Stores=$stores; CatalogDependencies=$dependencies }
    }
    return [pscustomobject]@{
        Schema=1; ReadOnly=$true; AutomaticRemovalEnabled=$false; InventoryCompleteAndStable=$complete
        Qualification='Advisory only; native Windows inventory qualification and fresh revalidation are required before any installer cleanup.'
        Decisions=$decisions; ProposedRemovals=$proposals; Before=$Before; After=$After
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    $before = Get-LegacyDriverTrustSnapshot
    $after = Get-LegacyDriverTrustSnapshot
    New-LegacyDriverTrustCleanupPlan -Before $before -After $after | ConvertTo-Json -Depth 16
}
