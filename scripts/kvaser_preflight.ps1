[CmdletBinding()]
param(
  [ValidateSet('List', 'Passive', 'Active')]
  [string]$Mode = 'List',

  [string]$Profile = '',

  [ValidateRange(100, 60000)]
  [uint32]$PassiveMs = 3000,

  [switch]$RequirePhysical,

  [switch]$AllowDiagnosticTransmit,

  [string]$TracePath = ''
)

$ErrorActionPreference = 'Stop'
$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDirectory

$probeCandidates = @(
  (Join-Path $scriptDirectory 'can_hardware_probe.exe'),
  (Join-Path $projectRoot 'build\nmake-x64\Release\can_hardware_probe.exe'),
  (Join-Path $projectRoot 'build\nmake-x64\Debug\can_hardware_probe.exe')
)
$probePath = $probeCandidates |
  Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
  Select-Object -First 1
if (-not $probePath) {
  throw "can_hardware_probe.exe not found. Checked: $($probeCandidates -join '; ')"
}

if ([string]::IsNullOrWhiteSpace($Profile)) {
  $Profile = Join-Path $projectRoot 'profiles\chuneng_331_left_rear.ini'
} elseif (-not [System.IO.Path]::IsPathRooted($Profile)) {
  $Profile = Join-Path $projectRoot $Profile
}
$Profile = [System.IO.Path]::GetFullPath($Profile)
if (-not (Test-Path -LiteralPath $Profile -PathType Leaf)) {
  throw "Profile not found: $Profile"
}

$driverCandidates = @()
if (-not [string]::IsNullOrWhiteSpace($env:UDS_KVASER_DRIVER_DIR)) {
  $driverCandidates +=
    (Join-Path $env:UDS_KVASER_DRIVER_DIR 'canlib32.dll')
}
$driverCandidates += 'C:\Windows\System32\canlib32.dll'
$driverPath = $driverCandidates |
  Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
  Select-Object -First 1
if ($driverPath) {
  $driver = Get-Item -LiteralPath $driverPath
  Write-Output "CANLIB_DLL=$driverPath"
  Write-Output "CANLIB_FILE_VERSION=$($driver.VersionInfo.FileVersion)"
  Write-Output "CANLIB_PRODUCT_VERSION=$($driver.VersionInfo.ProductVersion)"
} else {
  Write-Output 'CANLIB_DLL=MISSING'
}

Write-Output 'PNP_KVASER_BEGIN'
try {
  $pnpDevices = @(Get-PnpDevice -PresentOnly -ErrorAction Stop |
      Where-Object {
        $_.Class -eq 'CanDevices' -or $_.FriendlyName -match 'Kvaser'
      })
  if ($pnpDevices.Count -eq 0) {
    Write-Output 'PNP_KVASER=NONE'
  } else {
    $pnpDevices | ForEach-Object {
      Write-Output (
        'PNP_KVASER={0}|{1}|{2}' -f
        $_.Status, $_.FriendlyName, $_.InstanceId)
    }
  }
} catch {
  Write-Output "PNP_KVASER=UNAVAILABLE|$($_.Exception.Message)"
}
Write-Output 'PNP_KVASER_END'

function Invoke-KvaserProbe([string[]]$Arguments) {
  $output = @(& $probePath @Arguments 2>&1 |
      ForEach-Object { $_.ToString() })
  $exitCode = $LASTEXITCODE
  $output | ForEach-Object { Write-Host $_ }
  return @{
    ExitCode = $exitCode
    Text = $output -join "`n"
  }
}

$commonArguments = @(
  '--vendor', 'kvaser',
  '--profile', $Profile
)

Write-Output 'KVASER_ENUMERATION_BEGIN'
$enumeration = Invoke-KvaserProbe (
  $commonArguments + @('--passive-ms', '0'))
Write-Output 'KVASER_ENUMERATION_END'
if ($enumeration.ExitCode -ne 0) {
  [Console]::Error.WriteLine(
    "ERROR=Kvaser enumeration failed with exit code $($enumeration.ExitCode)")
  exit $enumeration.ExitCode
}

$physicalDetected = $enumeration.Text -match 'kind=PHYSICAL'
$virtualDetected = $enumeration.Text -match 'kind=VIRTUAL'
Write-Output "PHYSICAL_KVASER=$(
  if ($physicalDetected) { 'YES' } else { 'NO' })"
Write-Output "VIRTUAL_KVASER=$(
  if ($virtualDetected) { 'YES' } else { 'NO' })"

$physicalRequired = $RequirePhysical -or $Mode -ne 'List'
if ($physicalRequired -and -not $physicalDetected) {
  [Console]::Error.WriteLine(
    'No physical Kvaser channel was detected. Virtual CAN is not accepted ' +
    'for passive or active bench validation.')
  exit 3
}
if ($Mode -eq 'List') {
  Write-Output 'PREFLIGHT_RESULT=LIST_COMPLETE'
  exit 0
}

if ($Mode -eq 'Passive') {
  Write-Output 'KVASER_PASSIVE_BEGIN'
  $passive = Invoke-KvaserProbe (
    $commonArguments + @('--passive-ms', $PassiveMs.ToString()))
  Write-Output 'KVASER_PASSIVE_END'
  if ($passive.ExitCode -ne 0) {
    [Console]::Error.WriteLine(
      "ERROR=Kvaser passive probe failed with exit code $($passive.ExitCode)")
    exit $passive.ExitCode
  }
  Write-Output 'PREFLIGHT_RESULT=PASSIVE_COMPLETE'
  exit 0
}

if (-not $AllowDiagnosticTransmit) {
  throw (
    'Active mode sends only the Profile probe requests, but requires explicit ' +
    '-AllowDiagnosticTransmit authorization.')
}
if ([string]::IsNullOrWhiteSpace($TracePath)) {
  $validationDirectory = Join-Path $projectRoot 'validation\kvaser_bench'
  New-Item -ItemType Directory -Path $validationDirectory -Force | Out-Null
  $timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'
  $TracePath = Join-Path $validationDirectory "kvaser_$timestamp.asc"
} elseif (-not [System.IO.Path]::IsPathRooted($TracePath)) {
  $TracePath = Join-Path $projectRoot $TracePath
}
$TracePath = [System.IO.Path]::GetFullPath($TracePath)

Write-Output 'KVASER_ACTIVE_BEGIN'
$active = Invoke-KvaserProbe (
  $commonArguments +
  @('--passive-ms', '0', '--active', '--trace', $TracePath))
Write-Output 'KVASER_ACTIVE_END'
if ($active.ExitCode -ne 0) {
  [Console]::Error.WriteLine(
    "ERROR=Kvaser active probe failed with exit code $($active.ExitCode)")
  exit $active.ExitCode
}

$traceHash = Get-FileHash -LiteralPath $TracePath -Algorithm SHA256
Write-Output "TRACE=$TracePath"
Write-Output "TRACE_SHA256=$($traceHash.Hash)"
Write-Output 'PREFLIGHT_RESULT=ACTIVE_PASS'
