$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$source = Join-Path $repo 'src_assets/windows/misc/migration/installer-migrations.ps1'
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Installer migration parser failure'}
$utf8NoBom=[Text.UTF8Encoding]::new($false)
foreach($name in @('Read-Utf8TextFile','Write-Utf8TextFile','Set-SunshineConfigOption')) {
    $fn=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name}.GetNewClosure(),$true)
    . ([scriptblock]::Create($fn.Extent.Text))
}
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('installer-migration-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
try {
    $path=Join-Path $scratch 'sunshine.conf'
    $key='dd_use_sunshine_virtual_display_driver'
    $fixtures=@(
        "gamepad = vhf_ds5`r`n$key = enabled`r`n",
        "$key = enabled`n$key = enabled`n",
        "DD_USE_SUNSHINE_VIRTUAL_DISPLAY_DRIVER = enabled`n",
        "# Keep this comment`n$key = enabled # selected`n"
    )
    foreach($text in $fixtures) {
        [IO.File]::WriteAllText($path,$text,$utf8NoBom)
        for($attempt=0;$attempt -lt 3;$attempt++) {
            if(Set-SunshineConfigOption -ConfigPath $path -Name $key -Value 'enabled'){throw 'Unchanged selection rewrites/appends configuration'}
            if([IO.File]::ReadAllText($path) -cne $text){throw 'Repeated upgrade/repair changed configuration bytes'}
        }
    }
    [IO.File]::WriteAllText($path,"gamepad = vhf_ds5`n$key = disabled`n$key = enabled`n",$utf8NoBom)
    if(-not (Set-SunshineConfigOption -ConfigPath $path -Name $key -Value 'disabled')){throw 'Changed selection was ignored'}
    $changed=[IO.File]::ReadAllText($path)
    if($changed -notmatch 'gamepad = vhf_ds5' -or @([regex]::Matches($changed,"$key = disabled")).Count -ne 2){throw 'Changed selection lost another setting or left a conflicting duplicate'}
    if(Set-SunshineConfigOption -ConfigPath $path -Name $key -Value 'disabled'){throw 'Changed selection is not idempotent'}
    [IO.File]::WriteAllText($path,"gamepad = vhf_ds5`n",$utf8NoBom)
    if(-not (Set-SunshineConfigOption -ConfigPath $path -Name $key -Value 'enabled')){throw 'Missing selection was not appended'}
    if(Set-SunshineConfigOption -ConfigPath $path -Name $key -Value 'enabled'){throw 'New selection is duplicated on repair'}
    Write-Output 'Installer selection migration passed unchanged, duplicate, changed and missing-value upgrade/repair tests; no installed configuration was touched.'
} finally {
    $resolved=[IO.Path]::GetFullPath($scratch)
    $tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    if(-not $resolved.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolved) -notmatch '^installer-migration-test-[0-9a-f]{32}$'){throw 'Unsafe migration fixture cleanup path'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
