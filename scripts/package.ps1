param(
  [ValidateSet('Debug','Release')][string]$Config='Release',
  [string]$ReleaseName='UDS_Tool_Release.zip'
)

$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\')
$dist=Join-Path $root 'dist'
$releaseLeaf=[IO.Path]::GetFileName($ReleaseName)
if($releaseLeaf -ne $ReleaseName -or
   -not $releaseLeaf.EndsWith('.zip',[StringComparison]::OrdinalIgnoreCase)){
  throw 'ReleaseName must be a plain .zip filename'
}
$zip=Join-Path $root $releaseLeaf

& "$PSScriptRoot\build.ps1" -Config $Config -DistPath $dist
if($LASTEXITCODE -ne 0){ throw "Build failed with exit code $LASTEXITCODE" }

$forbidden=Get-ChildItem -LiteralPath $dist -Recurse -Force | Where-Object {
  $relative=[IO.Path]::GetRelativePath($dist,$_.FullName)
  $relative -match '(^|\\)(logs|Configuration|validation|tools|__pycache__)(\\|$)' -or
  $relative -match '\.partial$' -or
  $relative -match '\.(h|lib|pdb|ilk|exp|obj|py|ps1|bat)$' -or
  $relative -match '(^|\\)Reference(\\|$)' -or
  $relative -match '_reconstructed\.vbf$' -or
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
    & cmake -E remove -f $candidate
    if($LASTEXITCODE -ne 0){ throw "Failed to remove old release: $candidate" }
  }

Compress-Archive -Path "$dist\*" -DestinationPath $zip -CompressionLevel Optimal -Force
$hash=(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
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
  if($zipFiles.Count -ne $distFiles.Count){
    throw "Package count mismatch: dist=$($distFiles.Count), zip=$($zipFiles.Count)"
  }
} finally {
  $archive.Dispose()
}

Write-Host "Package: $zip"
Write-Host "SHA-256: $hash"
Write-Host "Files: $($distFiles.Count)"
