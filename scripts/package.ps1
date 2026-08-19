$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$dist=Join-Path $root 'dist-ui-professional'
$resolvedRoot=[IO.Path]::GetFullPath($root).TrimEnd('\')
$resolvedDist=[IO.Path]::GetFullPath($dist).TrimEnd('\')
if(-not $resolvedDist.StartsWith($resolvedRoot + '\',[StringComparison]::OrdinalIgnoreCase)){
  throw "Refusing to clean package directory outside project: $resolvedDist"
}
if(Test-Path -LiteralPath $resolvedDist){ Remove-Item -LiteralPath $resolvedDist -Recurse -Force }
& "$PSScriptRoot\build.ps1" -Config Release -DistPath $resolvedDist
$old=Join-Path (Split-Path -Parent $root) 'UDS_TOOLS-main\resources'
if(Test-Path $old){ Copy-Item -LiteralPath $old -Destination $dist -Recurse -Force }
Compress-Archive -Path "$resolvedDist\*" -DestinationPath "$root\UDSToolCpp_Windows.zip" -Force
Write-Host "Package: $root\UDSToolCpp_Windows.zip"
