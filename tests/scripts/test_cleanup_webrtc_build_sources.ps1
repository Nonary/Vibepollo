param(
  [string]$RepositoryRoot = ""
)

$ErrorActionPreference = "Stop"

if (-not $RepositoryRoot) {
  $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
}
$cleanupScript = Join-Path $RepositoryRoot "scripts\cleanup_webrtc_build_sources.ps1"
if (-not (Test-Path -LiteralPath $cleanupScript)) {
  throw "Cleanup script not found: $cleanupScript"
}

function Assert-True {
  param(
    [bool]$Condition,
    [string]$Message
  )

  if (-not $Condition) {
    throw $Message
  }
}

function New-WebrtcFixture {
  param(
    [string]$CaseRoot,
    [switch]$OutputInsideBuild,
    [switch]$MismatchedDll,
    [switch]$MissingSentinel
  )

  $buildDir = Join-Path $CaseRoot "src"
  $outDir = if ($OutputInsideBuild) {
    Join-Path $buildDir "retained-output"
  } else {
    Join-Path $CaseRoot "out"
  }

  New-Item -ItemType Directory -Path (Join-Path $buildDir "src\out\mingw") -Force | Out-Null
  New-Item -ItemType Directory -Path (Join-Path $outDir "include") -Force | Out-Null
  New-Item -ItemType Directory -Path (Join-Path $outDir "lib") -Force | Out-Null

  if (-not $MissingSentinel) {
    Set-Content -LiteralPath (Join-Path $buildDir ".gclient") -Value "solutions = []" -Encoding ASCII
  }
  Set-Content -LiteralPath (Join-Path $buildDir "src\BUILD.gn") -Value "group(`"default`") {}" -Encoding ASCII
  Set-Content -LiteralPath (Join-Path $buildDir "src\DEPS") -Value "vars = {}" -Encoding ASCII

  $builtBytes = [System.Text.Encoding]::ASCII.GetBytes("completed-webrtc-build")
  $stagedBytes = if ($MismatchedDll) {
    [System.Text.Encoding]::ASCII.GetBytes("different-webrtc-build")
  } else {
    $builtBytes
  }
  [System.IO.File]::WriteAllBytes((Join-Path $buildDir "src\out\mingw\libwebrtc.dll"), $builtBytes)
  [System.IO.File]::WriteAllBytes((Join-Path $outDir "lib\libwebrtc.dll"), $stagedBytes)
  [System.IO.File]::WriteAllBytes((Join-Path $outDir "lib\libwebrtc.dll.a"), [byte[]](1, 2, 3))
  Set-Content -LiteralPath (Join-Path $outDir "include\libwebrtc.h") -Value "#pragma once" -Encoding ASCII

  return [pscustomobject]@{
    BuildDir = $buildDir
    OutDir = $outDir
  }
}

function Assert-CleanupFails {
  param(
    [pscustomobject]$Fixture,
    [string]$ExpectedMessage
  )

  $failed = $false
  try {
    & $cleanupScript -BuildDir $Fixture.BuildDir -OutDir $Fixture.OutDir
  } catch {
    $failed = $true
    if ($_.Exception.Message -notlike "*$ExpectedMessage*") {
      throw "Expected cleanup failure containing '$ExpectedMessage', got '$($_.Exception.Message)'"
    }
  }

  Assert-True $failed "Cleanup unexpectedly succeeded."
  Assert-True (Test-Path -LiteralPath $Fixture.BuildDir) "Failed cleanup removed the build workspace."
}

$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) "vibeshine-webrtc-cleanup-$([System.Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $testRoot | Out-Null
try {
  $success = New-WebrtcFixture -CaseRoot (Join-Path $testRoot "success")
  Push-Location $success.BuildDir
  try {
    & $cleanupScript -BuildDir $success.BuildDir -OutDir $success.OutDir
  } finally {
    Pop-Location
  }
  Assert-True (-not (Test-Path -LiteralPath $success.BuildDir)) "Successful cleanup retained the build workspace."
  Assert-True (Test-Path -LiteralPath (Join-Path $success.OutDir "lib\libwebrtc.dll")) "Successful cleanup removed the staged SDK."
  & $cleanupScript -BuildDir $success.BuildDir -OutDir $success.OutDir

  $inside = New-WebrtcFixture -CaseRoot (Join-Path $testRoot "inside") -OutputInsideBuild
  Assert-CleanupFails -Fixture $inside -ExpectedMessage "must be outside"

  $mismatch = New-WebrtcFixture -CaseRoot (Join-Path $testRoot "mismatch") -MismatchedDll
  Assert-CleanupFails -Fixture $mismatch -ExpectedMessage "does not match"

  $missingSentinel = New-WebrtcFixture -CaseRoot (Join-Path $testRoot "missing-sentinel") -MissingSentinel
  Assert-CleanupFails -Fixture $missingSentinel -ExpectedMessage "missing or empty"

  Write-Host "WebRTC cleanup lifecycle tests passed."
} finally {
  if (Test-Path -LiteralPath $testRoot) {
    Remove-Item -LiteralPath $testRoot -Recurse -Force
  }
}
