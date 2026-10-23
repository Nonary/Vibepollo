param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path,
    [string]$PlannerScript
)
$ErrorActionPreference = 'Stop'
if (!$PlannerScript) { $PlannerScript=Join-Path $RepositoryRoot 'src_assets/windows/misc/migration/plan-legacy-driver-trust-cleanup.ps1' }
. $PlannerScript
$script:checks = 0
function Assert-That([bool]$Condition, [string]$Message) {
    $script:checks++
    if (!$Condition) { throw $Message }
}
$legacy = '37E100B2B382866FA7CE04EE33FE53DF53AEDC6D'
$sudo = '3C918FC73525AD8B1521B6DB26B71F694277CC49'
$gamepad = '7F1868BF86698A8674F40E686B02C8C702BD3B88'
$other = '0123456789ABCDEF0123456789ABCDEF01234567'
$publicSigner = '711AF71DC4C4952C8ED65BB4BA06826ED3922A32'
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('legacy-trust-plan-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
try {
    $script:fakeRoot = Join-Path $scratch 'Windows'
    $script:storeRoot = Join-Path $script:fakeRoot 'System32/DriverStore/FileRepository'
    $script:catRoot = Join-Path $script:fakeRoot 'System32/CatRoot'
    New-Item -ItemType Directory -Path $script:storeRoot, $script:catRoot -Force | Out-Null
    # Keep the actual recursive filesystem scanner and actual CMS verification.
    # Mock only native OS inventory and read-only trust-store boundaries.
    function Get-LegacyTrustNativeEnvironment {
        [pscustomobject]@{ WindowsRoot=$script:fakeRoot; CatalogRoots=@($script:storeRoot,$script:catRoot) }
    }
    function Get-LegacyTrustNativePendingReboot { return $script:pendingReboot }
    function Get-LegacyTrustNativePackages {
        if ($script:packagesFail) { throw 'DISM failed' }
        $script:fakePackages
    }
    function Get-LegacyTrustNativeDevices {
        if ($script:devicesFail) { throw 'CIM failed' }
        $script:fakeDevices
    }
    function Get-LegacyTrustNativeTrust {
        if ($script:trustFail) { throw 'Read-only store open failed' }
        $script:fakeTrust
    }
    function Reset-Fixture {
        Get-ChildItem -LiteralPath $script:storeRoot, $script:catRoot -Force | Remove-Item -Recurse -Force
        $script:fakePackages=@(); $script:fakeDevices=@()
        $script:packagesFail=$false; $script:devicesFail=$false; $script:trustFail=$false; $script:pendingReboot=$false
        $script:fakeTrust=@()
        foreach ($store in @('Root','TrustedPublisher')) {
            foreach ($pin in @($legacy,$sudo,$gamepad,$other)) {
                $script:fakeTrust += [pscustomobject]@{ Store=$store; Thumbprint=$pin }
            }
        }
    }
    function Add-FixturePackage([string]$Kind, [bool]$Active=$false) {
        $number = $script:fakePackages.Count + 1
        $dir = Join-Path $script:storeRoot "package-$number"
        New-Item -ItemType Directory -Path $dir | Out-Null
        switch ($Kind) {
            'legacy' { $payload='sunshine'; $inf='SunshineVirtualDisplayDriver.inf'; $cat='SunshineVirtualDisplayDriver.cat' }
            'sudo' { $payload='sudovda'; $inf='SudoVDA.inf'; $cat='SudoVDA.cat' }
            'unrelated' { $inf='machine.inf'; $cat='Microsoft.PowerShell.Archive.cat' }
            default { throw 'Unknown fixture package' }
        }
        if ($Kind -eq 'unrelated') {
            # Inbox INFs have no CatalogFile declaration. This public signed
            # non-driver catalog models an unrelated item in the global scan.
            '[Version]' | Set-Content -LiteralPath (Join-Path $dir $inf)
            Copy-Item -LiteralPath (Join-Path $RepositoryRoot 'packaging/windows/fixtures/legacy-driver-trust/Microsoft.PowerShell.Archive.cat') -Destination $dir
        } else {
            $source = Join-Path $RepositoryRoot "src_assets/windows/drivers/$payload"
            Copy-Item -LiteralPath (Join-Path $source $inf), (Join-Path $source $cat) -Destination $dir
        }
        $published="oem$number.inf"
        $script:fakePackages += [pscustomobject]@{ Driver=$published; OriginalFileName=(Join-Path $dir $inf); Inbox=($Kind -eq 'unrelated') }
        if ($Active) { $script:fakeDevices += [pscustomobject]@{ InfName=$published; DeviceID="ROOT\FIXTURE\$number" } }
        return Join-Path $dir $cat
    }
    function Invoke-Plan {
        $before = Get-LegacyDriverTrustSnapshot
        $after = Get-LegacyDriverTrustSnapshot
        New-LegacyDriverTrustCleanupPlan -Before $before -After $after
    }
    function Assert-Preserved($Plan, [string]$Reason) {
        Assert-That (!$Plan.InventoryCompleteAndStable -and @($Plan.ProposedRemovals).Count -eq 0) "$Reason must suppress every proposal"
        Assert-That (@($Plan.Decisions | Where-Object { $_.Decision -ne 'Preserve' }).Count -eq 0) "$Reason must preserve both pins"
    }

    Reset-Fixture
    $catalog = Add-FixturePackage 'unrelated' $true
    $trustBefore = $script:fakeTrust | ConvertTo-Json -Compress
    $plan = Invoke-Plan
    Assert-That $plan.InventoryCompleteAndStable 'Complete unchanged active/staged inventory must admit advisory proposals'
    Assert-That ($plan.ReadOnly -and !$plan.AutomaticRemovalEnabled) 'Advisory plan must never enable automatic removal'
    Assert-That (@($plan.ProposedRemovals).Count -eq 4) 'Only both exact legacy pins in both stores should be proposed'
    Assert-That (@($plan.ProposedRemovals | Where-Object { $_.Thumbprint -notin @($legacy,$sudo) -or $_.Store -notin @('LocalMachine\Root','LocalMachine\TrustedPublisher') }).Count -eq 0) 'Unrelated/gamepad/production certificates must never enter removal proposals'
    Assert-That (($script:fakeTrust | ConvertTo-Json -Compress) -ceq $trustBefore) 'Planning must not change trust fixtures'
    Assert-That ((Get-LegacyTrustCatalogIdentity $catalog).SignerThumbprints -contains $publicSigner) 'Production CMS reader must verify the existing signed unrelated catalog'

    foreach ($active in @($false,$true)) {
        Reset-Fixture
        $null = Add-FixturePackage 'unrelated' $true
        $null = Add-FixturePackage 'legacy' $active
        $plan=Invoke-Plan
        Assert-That ($plan.InventoryCompleteAndStable -and @($plan.ProposedRemovals).Count -eq 2) "An active=$active legacy package must preserve its pin only"
        Assert-That (($plan.Decisions | Where-Object Thumbprint -eq $legacy).Reason -eq 'CatalogDependency') 'Package version/product selection must not override signer dependency'
        Assert-That (@($plan.ProposedRemovals | Where-Object Thumbprint -eq $legacy).Count -eq 0) 'Dependent legacy trust must survive'
    }
    Reset-Fixture
    $null=Add-FixturePackage 'sudo' $false
    $plan=Invoke-Plan
    Assert-That (@($plan.ProposedRemovals | Where-Object Thumbprint -eq $sudo).Count -eq 0) 'An unselected staged SudoVDA package must preserve Sudo trust'

    Reset-Fixture
    $null=Add-FixturePackage 'unrelated' $true
    Copy-Item -LiteralPath (Join-Path $RepositoryRoot 'src_assets/windows/drivers/sunshine/SunshineVirtualDisplayDriver.cat') -Destination (Join-Path $script:catRoot 'retained-independent-package.cat')
    $plan=Invoke-Plan
    Assert-That (($plan.Decisions | Where-Object Thumbprint -eq $legacy).Decision -eq 'Preserve') 'A registered legacy catalog outside product package paths must preserve shared trust'

    foreach ($failure in @('pending-reboot','packages','devices','trust','catalog','missing-catalog','active-unmapped','active-null','unsafe-inf','missing-inf','unsigned-inf','unknown-inbox','duplicate-package','unknown-catalog-syntax')) {
        Reset-Fixture
        $catalog=Add-FixturePackage 'unrelated' $true
        switch ($failure) {
            'pending-reboot' { $script:pendingReboot=$true }
            'packages' { $script:packagesFail=$true }
            'devices' { $script:devicesFail=$true }
            'trust' { $script:trustFail=$true }
            'catalog' { [IO.File]::WriteAllBytes($catalog, [byte[]]@(1,2,3,4)) }
            'missing-catalog' { Remove-Item -LiteralPath $catalog }
            'active-unmapped' { $script:fakeDevices[0].InfName='oem999.inf' }
            'active-null' { $script:fakeDevices[0].InfName=$null }
            'unsafe-inf' { $script:fakePackages[0].OriginalFileName=Join-Path $scratch 'outside.inf' }
            'missing-inf' { Remove-Item -LiteralPath $script:fakePackages[0].OriginalFileName }
            'unsigned-inf' { $script:fakePackages[0].Inbox=$false; '[Version]' | Set-Content -LiteralPath $script:fakePackages[0].OriginalFileName }
            'unknown-inbox' { $script:fakePackages[0].Inbox='False' }
            'duplicate-package' { $script:fakePackages += $script:fakePackages[0] }
            'unknown-catalog-syntax' { "[Version]`nCatalogFile=%catalog%" | Set-Content -LiteralPath $script:fakePackages[0].OriginalFileName }
        }
        Assert-Preserved (Invoke-Plan) $failure
    }
    Reset-Fixture
    $null=Add-FixturePackage 'unrelated' $true
    $before=Get-LegacyDriverTrustSnapshot
    $script:fakeTrust=@($script:fakeTrust | Where-Object { $_.Thumbprint -ne $other })
    Assert-Preserved (New-LegacyDriverTrustCleanupPlan $before (Get-LegacyDriverTrustSnapshot)) 'Trust changed during collection'
    Reset-Fixture
    $null=Add-FixturePackage 'unrelated' $true
    $before=Get-LegacyDriverTrustSnapshot
    $null=Add-FixturePackage 'legacy' $false
    Assert-Preserved (New-LegacyDriverTrustCleanupPlan $before (Get-LegacyDriverTrustSnapshot)) 'A package staged during collection'

    Reset-Fixture
    $null=Add-FixturePackage 'unrelated' $true
    $script:fakeTrust=@($script:fakeTrust | Where-Object { $_.Thumbprint -notin @($legacy,$sudo) })
    $plan=Invoke-Plan
    Assert-That ($plan.InventoryCompleteAndStable -and @($plan.ProposedRemovals).Count -eq 0) 'Absent legacy certificates must not create removal entries'
    Assert-That (@($plan.Decisions | Where-Object Decision -eq 'Absent').Count -eq 2) 'Absent identities must be reported explicitly'

    Reset-Fixture
    $null=Add-FixturePackage 'unrelated' $true
    $snapshot=Get-LegacyDriverTrustSnapshot
    $snapshot.Catalogs[0].SignerThumbprints=@('bad identity')
    Assert-Preserved (New-LegacyDriverTrustCleanupPlan $snapshot $snapshot) 'Malformed signature evidence'
    Assert-Preserved (New-LegacyDriverTrustCleanupPlan ([pscustomobject]@{Complete=$true}) ([pscustomobject]@{Complete=$true})) 'Unknown snapshot schema'

    foreach ($failure in @('package-shape','active-shape','catalog-duplicate','complete-string')) {
        Reset-Fixture
        $null=Add-FixturePackage 'unrelated' $true
        $snapshot=Get-LegacyDriverTrustSnapshot
        switch ($failure) {
            'package-shape' { $snapshot.Packages[0].PSObject.Properties.Remove('InfSha256') }
            'active-shape' { $snapshot.Active[0].PublishedInf='oem999.inf' }
            'catalog-duplicate' { $snapshot.Catalogs += $snapshot.Catalogs[0] }
            'complete-string' { $snapshot.Complete='true' }
        }
        Assert-Preserved (New-LegacyDriverTrustCleanupPlan $snapshot $snapshot) $failure
    }
    Reset-Fixture
    $null=Add-FixturePackage 'legacy' $true
    $package=$script:fakePackages[0]
    $extra=Join-Path (Split-Path -Parent $package.OriginalFileName) 'alternate.cat'
    Copy-Item -LiteralPath (Join-Path $RepositoryRoot 'packaging/windows/fixtures/legacy-driver-trust/Microsoft.PowerShell.Archive.cat') -Destination $extra
    Add-Content -LiteralPath $package.OriginalFileName -Value "`n[Version]`nCatalogFile.ntamd64=alternate.cat"
    Assert-Preserved (Invoke-Plan) 'Duplicate Version section'

    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        Reset-Fixture
        $null=Add-FixturePackage 'unrelated' $true
        $target=Join-Path $scratch 'reparse-target'
        New-Item -ItemType Directory -Path $target | Out-Null
        $link=Join-Path $script:catRoot 'unread-subtree'
        New-Item -ItemType SymbolicLink -Path $link -Target $target | Out-Null
        Assert-Preserved (Invoke-Plan) 'Reparse-point catalog subtree'
        Remove-Item -LiteralPath $link -Force
    }

    # Cryptographic negative control: change authenticated catalog content,
    # retaining enough ASN.1 structure for SignedCms.Decode to succeed.
    Reset-Fixture
    $catalog=Add-FixturePackage 'legacy' $true
    $bytes=[IO.File]::ReadAllBytes($catalog)
    $cms=New-Object Security.Cryptography.Pkcs.SignedCms; $cms.Decode($bytes)
    $content=$cms.ContentInfo.Content
    $index=-1
    for ($i=0; $i -le $bytes.Length-$content.Length; $i++) {
        if ($bytes[$i] -eq $content[0] -and $bytes[$i+1] -eq $content[1]) {
            $same=$true
            for ($j=0; $j -lt $content.Length; $j++) { if ($bytes[$i+$j] -ne $content[$j]) { $same=$false; break } }
            if ($same) { $index=$i; break }
        }
    }
    Assert-That ($index -ge 0) 'Locate signed content in existing public catalog fixture'
    $bytes[$index+$content.Length-1]=$bytes[$index+$content.Length-1] -bxor 1
    [IO.File]::WriteAllBytes($catalog,$bytes)
    $decoded=New-Object Security.Cryptography.Pkcs.SignedCms; $decoded.Decode($bytes)
    $rejected=$false; try { $null=Get-LegacyTrustCatalogIdentity $catalog } catch { $rejected=$true }
    Assert-That $rejected 'Changed signed bytes must fail production CMS verification'
    Assert-Preserved (Invoke-Plan) 'Cryptographically modified catalog'

    Write-Host "$script:checks behavioral checks passed. Real catalog CMS signatures and recursive catalog discovery exercised; Windows DISM/CIM/read-only trust boundaries mocked. No certificates imported/removed or drivers executed."
} finally { Remove-Item -LiteralPath $scratch -Recurse -Force }
