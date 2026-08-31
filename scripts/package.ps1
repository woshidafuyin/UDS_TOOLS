param(
  [ValidateSet('Debug','Release')][string]$Config='Release',
  [string]$ReleaseName='UDS_Tool_Release.zip',
  [string]$DistPath='',
  [string]$BuildRoot='',
  [string]$VisualStudioRoot='',
  [string]$QtRoot='',
  [string]$CMakePath=''
)

$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\')

function Get-Sha256Hex([string]$Path) {
  $stream=[IO.File]::OpenRead($Path)
  try {
    return Get-StreamSha256Hex $stream
  } finally {
    $stream.Dispose()
  }
}

function Get-StreamSha256Hex([IO.Stream]$Stream) {
  $sha=[Security.Cryptography.SHA256]::Create()
  try {
    return -join ($sha.ComputeHash($Stream) | ForEach-Object {
      $_.ToString('X2')
    })
  } finally {
    $sha.Dispose()
  }
}

function Get-RelativePath([string]$BasePath,[string]$TargetPath) {
  $base=[IO.Path]::GetFullPath($BasePath).TrimEnd('\') + '\'
  $target=[IO.Path]::GetFullPath($TargetPath)
  if(-not $target.StartsWith($base,[StringComparison]::OrdinalIgnoreCase)){
    throw "Path is outside the expected directory: $target"
  }
  return $target.Substring($base.Length)
}

$dist=if([string]::IsNullOrWhiteSpace($DistPath)){
  Join-Path $root 'dist'
} elseif([IO.Path]::IsPathRooted($DistPath)){
  [IO.Path]::GetFullPath($DistPath)
} else {
  [IO.Path]::GetFullPath((Join-Path $root $DistPath))
}
if($dist -eq $root -or
   -not $dist.StartsWith($root + '\',[StringComparison]::OrdinalIgnoreCase)){
  throw "DistPath must remain under the repository: $dist"
}
$runningFromDist=@(Get-Process -ErrorAction SilentlyContinue | ForEach-Object {
  $processPath=''
  try { $processPath=$_.Path } catch { return }
  if($processPath -and
     $processPath.StartsWith($dist + '\',[StringComparison]::OrdinalIgnoreCase)){
    [PSCustomObject]@{ Name=$_.ProcessName; Id=$_.Id; Path=$processPath }
  }
})
if($runningFromDist){
  $details=($runningFromDist | ForEach-Object {
    "$($_.Name) (PID $($_.Id)): $($_.Path)"
  }) -join [Environment]::NewLine
  throw "Cannot update dist because a delivered program is still running. Close it and retry:$([Environment]::NewLine)$details"
}
$releaseLeaf=[IO.Path]::GetFileName($ReleaseName)
if($releaseLeaf -ne $ReleaseName -or
   -not $releaseLeaf.EndsWith('.zip',[StringComparison]::OrdinalIgnoreCase)){
  throw 'ReleaseName must be a plain .zip filename'
}
$zip=Join-Path $root $releaseLeaf

$buildArguments=@{
  Config=$Config
  DistPath=$dist
}
foreach($optionalArgument in @{
  BuildRoot=$BuildRoot
  VisualStudioRoot=$VisualStudioRoot
  QtRoot=$QtRoot
  CMakePath=$CMakePath
}.GetEnumerator()){
  if(-not [string]::IsNullOrWhiteSpace($optionalArgument.Value)){
    $buildArguments[$optionalArgument.Key]=$optionalArgument.Value
  }
}
& "$PSScriptRoot\build.ps1" @buildArguments
if($LASTEXITCODE -ne 0){ throw "Build failed with exit code $LASTEXITCODE" }

$forbidden=Get-ChildItem -LiteralPath $dist -Recurse -Force | Where-Object {
  $relative=Get-RelativePath $dist $_.FullName
  $relative -match '(^|\\)(logs|Configuration|validation|tools|__pycache__)(\\|$)' -or
  $relative -match '\.partial$' -or
  $relative -match '\.(h|lib|pdb|ilk|exp|obj|py|ps1|bat)$' -or
  $relative -match '(^|\\)Reference(\\|$)' -or
  $relative -match '(^|\\)(SOURCE_MANIFEST|RESOURCE_MANIFEST|PROVENANCE|BASELINE)' -or
  $relative -match 'README_CHUNENG_.*_EDITION\.md$'
}
if($forbidden){
  $details=($forbidden.FullName -join [Environment]::NewLine)
  throw "Forbidden delivery artifacts found:$([Environment]::NewLine)$details"
}

$profileReferences=Get-ChildItem -LiteralPath (Join-Path $dist 'profiles') -Filter '*.ini' -File |
  ForEach-Object {
    Select-String -LiteralPath $_.FullName -Pattern '^[^;#=]*(?:file|dll)=(.+)$' |
      ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() } |
      Where-Object { $_ }
  } | Sort-Object -Unique
$missingReferences=@($profileReferences | Where-Object {
  -not (Test-Path -LiteralPath (Join-Path $dist $_) -PathType Leaf)
})
if($missingReferences){
  throw "Profile runtime files missing:$([Environment]::NewLine)$($missingReferences -join [Environment]::NewLine)"
}

Get-ChildItem -LiteralPath $root -File -Filter 'UDS_Tool_Release*.zip*' |
  ForEach-Object {
    $candidate=[IO.Path]::GetFullPath($_.FullName)
    if(-not $candidate.StartsWith($root + '\',[StringComparison]::OrdinalIgnoreCase)){
      throw "Refusing to remove release outside project: $candidate"
    }
    Remove-Item -LiteralPath $candidate -Force
  }

Compress-Archive -Path "$dist\*" -DestinationPath $zip -CompressionLevel Optimal -Force
$hash=Get-Sha256Hex $zip
$sidecar="$zip.sha256.txt"
[IO.File]::WriteAllText(
  $sidecar,
  "$hash  $releaseLeaf$([Environment]::NewLine)",
  [Text.UTF8Encoding]::new($false))

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive=[IO.Compression.ZipFile]::OpenRead($zip)
try {
  $zipFiles=@($archive.Entries | Where-Object { $_.Name -ne '' })
  $distFiles=@(Get-ChildItem -LiteralPath $dist -Recurse -File -Force)
  $distHashes=@{}
  foreach($file in $distFiles){
    $relative=(Get-RelativePath $dist $file.FullName).Replace('\','/')
    $distHashes[$relative]=Get-Sha256Hex $file.FullName
  }
  $zipHashes=@{}
  foreach($entry in $zipFiles){
    $stream=$entry.Open()
    try {
      $zipHashes[$entry.FullName.Replace('\','/')]=Get-StreamSha256Hex $stream
    } finally {
      $stream.Dispose()
    }
  }
  $missing=@($distHashes.Keys | Where-Object { -not $zipHashes.ContainsKey($_) })
  $extra=@($zipHashes.Keys | Where-Object { -not $distHashes.ContainsKey($_) })
  $changed=@($distHashes.Keys | Where-Object {
    $zipHashes.ContainsKey($_) -and $distHashes[$_] -ne $zipHashes[$_]
  })
  if($missing -or $extra -or $changed){
    throw "Package differs from dist: missing=$($missing.Count), extra=$($extra.Count), hash mismatch=$($changed.Count)"
  }
} finally {
  $archive.Dispose()
}

Write-Host "Package: $zip"
Write-Host "SHA-256: $hash"
Write-Host "Files: $($distFiles.Count)"
