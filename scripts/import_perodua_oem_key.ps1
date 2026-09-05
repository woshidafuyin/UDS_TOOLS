param(
  [string]$OutputPath = (Join-Path $env:LOCALAPPDATA 'ChuHang\DiagnosticStudio\keys\perodua_p02c_level4.key')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
# Enter only the CPD Level4 column, in table order. CES009 and OEM Key List
# specify right-padding a short key with zero bytes to reach 16 bytes.
# The prompt keeps the key out of command history and diagnostic logs.
$entered = Read-Host 'Perodua CPD Level4 OEM key (hex, 1..16 bytes)' -AsSecureString
$ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($entered)
$secret = $null
try {
  $hexText = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr)
  $hexText = ($hexText -replace '(?i)0x','' -replace '[\s,]','')
  if ($hexText -notmatch '^(?:[0-9a-fA-F]{2}){1,16}$') {
    throw 'Expected 1..16 complete hex bytes from the CPD Level4 column.'
  }
  $secret = New-Object byte[] 16
  for ($i = 0; $i -lt $hexText.Length / 2; $i++) {
    $secret[$i] = [Convert]::ToByte($hexText.Substring($i * 2, 2), 16)
  }
  $encrypted = [Security.Cryptography.ProtectedData]::Protect(
    $secret, $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
  $path = [IO.Path]::GetFullPath($OutputPath)
  [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
  [IO.File]::WriteAllBytes($path, ([Text.Encoding]::ASCII.GetBytes('CHKEY1') + $encrypted))
  Write-Host "Imported protected Perodua Level4 key for the current Windows user: $path"
} finally {
  [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr)
  if ($secret) { [Array]::Clear($secret, 0, $secret.Length) }
  $hexText = $null
  $entered.Dispose()
}
