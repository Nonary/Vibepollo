$bytes = [byte[]]::new(10)
$rng = [Security.Cryptography.RandomNumberGenerator]::Create()
try {
  $rng.GetBytes($bytes)
} finally {
  if ($null -ne $rng) {
    $rng.Dispose()
  }
}

$milliseconds = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$timestamp = '{0:x12}' -f $milliseconds
$randomHex = ($bytes | ForEach-Object { $_.ToString('x2') }) -join ''

# UUIDv7 layout: 48-bit Unix millisecond timestamp, version nibble 7,
# RFC 4122 variant nibble 8-B, then random bits.
$variant = [Convert]::ToString((([Convert]::ToInt32($randomHex.Substring(3, 1), 16) -band 3) -bor 8), 16)
$guid = ('{0}-{1}-7{2}-{3}{4}-{5}' -f `
  $timestamp.Substring(0, 8), `
  $timestamp.Substring(8, 4), `
  $randomHex.Substring(0, 3), `
  $variant, `
  $randomHex.Substring(4, 3), `
  $randomHex.Substring(7, 12)).ToUpperInvariant()

'{' + $guid + '}'
