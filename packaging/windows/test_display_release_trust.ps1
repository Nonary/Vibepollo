param([string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path)
$ErrorActionPreference='Stop'
function Assert-That($Condition,$Message) { if (!$Condition) { throw $Message } }
function Load-Function($Ast,$Name) {
    $node=$Ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $Name}.GetNewClosure(),$true)
    if (!$node) { throw "Missing function: $Name" }
    # Return function code; caller loads it in test scope, never installer body.
    return $node.Extent.Text
}
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('display-release-trust-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $scratch | Out-Null
try {
    foreach ($driver in @('sunshine','sudovda')) {
        $folder=Join-Path $RepositoryRoot "src_assets/windows/drivers/$driver"
        $tokens=$null; $errors=$null
        $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $folder 'install.ps1'),[ref]$tokens,[ref]$errors)
        Assert-That (!$errors.Count) 'Display script must parse'
        foreach ($name in @('Initialize-DisplayPackageValidation','Assert-ExpectedDisplayPackage','Install-ExpectedDisplayTrust','Undo-DisplayTrustProvisioning')) {
            . ([scriptblock]::Create((Load-Function $ast $name)))
        }
        $prefix=if ($driver -eq 'sudovda') {'SudoVDA'} else {'SunshineVirtualDisplayDriver'}
        $hardware=if ($driver -eq 'sudovda') {'ROOT\SUDOMAKER\SUDOVDA'} else {'Root\SunshineVirtualDisplay'}
        $script:infPath=Join-Path $folder "$prefix.inf"; $script:catPath=Join-Path $folder "$prefix.cat"
        $script:dllPath=Join-Path $folder "$prefix.dll"; $script:certPath=Join-Path $folder "$prefix.cer"; $script:hardwareId=$hardware
        $verified=Assert-ExpectedDisplayPackage -InfPath $infPath -CatalogPath $catPath -HardwareId $hardware -DriverPath $dllPath -CertificatePath $certPath
        Assert-That ($verified.SelfSigned -and !$verified.Certificate.HasPrivateKey) 'Real expected selfsigned public fixture must verify'
        # Inert scratch copies only; changing a catalog-bound byte must fail.
        foreach ($member in @($infPath,$dllPath,$catPath)) {
            $copy=Join-Path $scratch ([IO.Path]::GetFileName($member))
            $bytes=[IO.File]::ReadAllBytes($member)
            # PE certificate-table padding is outside its authenticated image hash.
            # Alter actual image content rather than treating padding as payload.
            $offset=if ($member -eq $dllPath) {512} else {0}
            $bytes[$offset]=$bytes[$offset] -bxor 1
            [IO.File]::WriteAllBytes($copy,$bytes)
            $args=@{InfPath=$infPath;CatalogPath=$catPath;HardwareId=$hardware;DriverPath=$dllPath;CertificatePath=$certPath}
            if ($member -eq $infPath) {$args.InfPath=$copy} elseif ($member -eq $dllPath) {$args.DriverPath=$copy} else {$args.CatalogPath=$copy}
            $rejected=$false; try {$null=Assert-ExpectedDisplayPackage @args} catch {$rejected=$true}
            Assert-That $rejected "Tampered display member/catalog must fail before trust: $member"
        }
        $other=if ($driver -eq 'sudovda') {'sunshine/SunshineVirtualDisplayDriver.cer'} else {'sudovda/SudoVDA.cer'}
        $rejected=$false
        try {$null=Assert-ExpectedDisplayPackage -InfPath $infPath -CatalogPath $catPath -HardwareId $hardware -DriverPath $dllPath -CertificatePath (Join-Path $RepositoryRoot "src_assets/windows/drivers/$other")} catch {$rejected=$true}
        Assert-That $rejected 'Substituted certificate must not redefine expected signer'
        $otherCat=if ($driver -eq 'sudovda') {'sunshine/SunshineVirtualDisplayDriver.cat'} else {'sudovda/SudoVDA.cat'}
        $substitutedCat=Join-Path $scratch "$prefix.cat"
        Copy-Item -LiteralPath (Join-Path $RepositoryRoot "src_assets/windows/drivers/$otherCat") -Destination $substitutedCat -Force
        $rejected=$false
        try {$null=Assert-ExpectedDisplayPackage -InfPath $infPath -CatalogPath $substitutedCat -HardwareId $hardware -DriverPath $dllPath -CertificatePath $certPath} catch {$rejected=$true}
        Assert-That $rejected 'Cryptographically signed catalog from the wrong producer must fail'
        $rejected=$false
        try {$null=Assert-ExpectedDisplayPackage -InfPath $infPath -CatalogPath $catPath -HardwareId 'ROOT\WRONG' -DriverPath $dllPath -CertificatePath $certPath} catch {$rejected=$true}
        Assert-That $rejected 'Wrong hardware identity must fail'
        # All certificate operations below are mocks. Never load store functions.
        $script:mockPackage=$verified; $script:mockStores=@{}; $script:failStore=''; $script:badPostTrust=$false; $script:rejectPackage=$false
        function Assert-ExpectedDisplayPackage { if ($script:rejectPackage) {throw 'Mock invalid package'}; return $script:mockPackage }
        function Test-DisplayCertificatePresent {param($Certificate,$StoreName) return $script:mockStores.ContainsKey($StoreName)}
        function Add-DisplayCertificate {param($Certificate,$StoreName) $script:mockStores[$StoreName]=$Certificate.Thumbprint; if ($StoreName -eq $script:failStore) {throw 'Mock partial add failure'}}
        function Remove-DisplayCertificate {param($Certificate,$StoreName) $script:mockStores.Remove($StoreName)}
        function Get-AuthenticodeSignature {param($LiteralPath,$ErrorAction) return [pscustomobject]@{Status=$(if($script:badPostTrust){'NotTrusted'}else{'Valid'});SignerCertificate=$script:mockPackage.Certificate}}
        Install-ExpectedDisplayTrust
        Assert-That ($script:mockStores.Count -eq 2) 'Expected selfsigned fresh install must provision exact Root and publisher'
        $stage=@($ast.EndBlock.Statements | Where-Object {
            $_ -is [Management.Automation.Language.TryStatementAst] -and $_.Extent.Text.Contains("@('/add-driver', ")
        })
        Assert-That ($stage.Count -eq 1) 'Exactly one pre-mutation Windows staging boundary required'
        function Invoke-DriverStep { param($FilePath,$ArgumentList,$Description) throw 'Mock Windows staging rejection' }
        function Invoke-DriverProcess { param($FilePath,$ArgumentList,$AllowedExitCodes) throw 'Mock Windows staging rejection' }
        $rejected=$false; try { . ([scriptblock]::Create($stage[0].Extent.Text)) } catch {$rejected=$true}
        Assert-That ($rejected -and $script:mockStores.Count -eq 0) 'Windows staging rejection must undo newly provisioned trust before driver replacement'
        Install-ExpectedDisplayTrust
        Undo-DisplayTrustProvisioning
        Assert-That ($script:mockStores.Count -eq 0) 'Owned trust additions must roll back'
        $script:mockStores['Root']=$verified.Certificate.Thumbprint; $script:failStore='TrustedPublisher'
        $rejected=$false; try {Install-ExpectedDisplayTrust} catch {$rejected=$true}
        Assert-That ($rejected -and $script:mockStores.Count -eq 1 -and $script:mockStores.ContainsKey('Root')) 'Partial publisher failure must preserve prior Root and undo new publisher'
        $script:mockStores=@{}; $script:failStore=''; $script:badPostTrust=$true
        $rejected=$false; try {Install-ExpectedDisplayTrust} catch {$rejected=$true}
        Assert-That ($rejected -and $script:mockStores.Count -eq 0) 'Failed final Windows trust check must roll back new trust'
        $script:badPostTrust=$false; $script:rejectPackage=$true
        $rejected=$false; try {Install-ExpectedDisplayTrust} catch {$rejected=$true}
        Assert-That ($rejected -and $script:mockStores.Count -eq 0) 'Wrong signer/integrity must fail before provisioning'
        $script:rejectPackage=$false; $script:mockPackage=[pscustomobject]@{Certificate=$verified.Certificate;SelfSigned=$false;CatalogPath='mock.cat'}
        Install-ExpectedDisplayTrust
        Assert-That ($script:mockStores.Count -eq 1 -and $script:mockStores.ContainsKey('TrustedPublisher')) 'Already trusted production signer must never be added to Root'
        Undo-DisplayTrustProvisioning
        Remove-Item Function:Get-AuthenticodeSignature
    }
    Write-Host 'Display release identity, real catalog/member tamper rejection and mocked trust transaction checks passed. No real trust or driver mutation.'
} finally {
    foreach ($file in Get-ChildItem -LiteralPath $scratch -File) { Remove-Item -LiteralPath $file.FullName }
    Remove-Item -LiteralPath $scratch
}
