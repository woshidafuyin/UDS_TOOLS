param(
  [ValidateSet('Debug','Release')][string]$Config='Release',
  [string]$DistPath='',
  [string]$BuildRoot='',
  [string]$VisualStudioRoot='',
  [string]$QtRoot='',
  [string]$CMakePath='',
  [switch]$ValidateEnvironmentOnly
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$requiredQtVersion='5.15.2'

function Resolve-InputPath([string]$Path,[string]$BasePath) {
  if([IO.Path]::IsPathRooted($Path)){
    return [IO.Path]::GetFullPath($Path)
  }
  return [IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function Resolve-CommandPath([string]$Name) {
  $command=Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue |
    Select-Object -First 1
  if($command){ return $command.Source }
  return ''
}

function Get-Sha256Hex([string]$Path) {
  $stream=[IO.File]::OpenRead($Path)
  try {
    $sha=[Security.Cryptography.SHA256]::Create()
    try {
      return -join ($sha.ComputeHash($stream) | ForEach-Object {
        $_.ToString('X2')
      })
    } finally {
      $sha.Dispose()
    }
  } finally {
    $stream.Dispose()
  }
}

function Resolve-VisualStudioRoot([string]$RequestedRoot) {
  if(-not [string]::IsNullOrWhiteSpace($RequestedRoot)){
    $resolved=Resolve-InputPath $RequestedRoot $root
    if(Test-Path -LiteralPath (Join-Path $resolved 'Common7\Tools\VsDevCmd.bat')){
      return $resolved
    }
    throw "Visual Studio C++ tools were not found under: $resolved"
  }

  $vswhereCandidates=@()
  $vswhereFromPath=Resolve-CommandPath 'vswhere.exe'
  if($vswhereFromPath){ $vswhereCandidates += $vswhereFromPath }
  $programFilesX86=[Environment]::GetFolderPath('ProgramFilesX86')
  if($programFilesX86){
    $vswhereCandidates += Join-Path $programFilesX86 `
      'Microsoft Visual Studio\Installer\vswhere.exe'
  }
  foreach($vswhere in $vswhereCandidates | Select-Object -Unique){
    if(-not (Test-Path -LiteralPath $vswhere)){ continue }
    $installation=& $vswhere -latest -products '*' `
      -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
      -property installationPath 2>$null | Select-Object -First 1
    if($installation -and
       (Test-Path -LiteralPath (Join-Path $installation `
                                      'Common7\Tools\VsDevCmd.bat'))){
      return [IO.Path]::GetFullPath($installation)
    }
  }
  throw 'Visual Studio with the MSVC x86/x64 C++ tools was not found. Install the C++ desktop workload or pass -VisualStudioRoot.'
}

function Resolve-CMakeExecutable([string]$RequestedPath,[string]$VsRoot) {
  if(-not [string]::IsNullOrWhiteSpace($RequestedPath)){
    $resolved=Resolve-InputPath $RequestedPath $root
    if(Test-Path -LiteralPath $resolved -PathType Container){
      $resolved=Join-Path $resolved 'cmake.exe'
    }
    if(Test-Path -LiteralPath $resolved -PathType Leaf){ return $resolved }
    throw "CMake executable was not found: $resolved"
  }
  $fromPath=Resolve-CommandPath 'cmake.exe'
  if($fromPath){ return $fromPath }
  $bundled=Join-Path $VsRoot `
    'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
  if(Test-Path -LiteralPath $bundled){ return $bundled }
  throw 'CMake was not found in PATH or the selected Visual Studio installation. Pass -CMakePath.'
}

function Resolve-QtRoot([string]$RequestedRoot) {
  $candidates=@()
  if(-not [string]::IsNullOrWhiteSpace($RequestedRoot)){
    $candidates += Resolve-InputPath $RequestedRoot $root
  } else {
    foreach($environmentRoot in @($env:QTDIR,$env:Qt5_DIR)){
      if([string]::IsNullOrWhiteSpace($environmentRoot)){ continue }
      $candidate=$environmentRoot
      if((Split-Path -Leaf $candidate) -eq 'Qt5'){
        $candidate=Split-Path -Parent (Split-Path -Parent `
          (Split-Path -Parent $candidate))
      }
      $candidates += $candidate
    }
    foreach($toolName in @('qmake.exe','windeployqt.exe')){
      $tool=Resolve-CommandPath $toolName
      if($tool){ $candidates += Split-Path -Parent (Split-Path -Parent $tool) }
    }
    $qtBases=@((Join-Path ([Environment]::GetFolderPath('UserProfile')) 'Qt'))
    Get-PSDrive -PSProvider FileSystem | ForEach-Object {
      $qtBases += Join-Path $_.Root 'Qt'
    }
    foreach($qtBase in $qtBases | Select-Object -Unique){
      if(-not (Test-Path -LiteralPath $qtBase -PathType Container)){ continue }
      Get-ChildItem -LiteralPath (Join-Path $qtBase $requiredQtVersion) `
                    -Directory -Filter 'msvc*_64' -ErrorAction SilentlyContinue |
        ForEach-Object { $candidates += $_.FullName }
    }
  }

  foreach($candidate in $candidates | Where-Object { $_ } | Select-Object -Unique){
    $resolved=[IO.Path]::GetFullPath($candidate)
    $qmake=Join-Path $resolved 'bin\qmake.exe'
    $deploy=Join-Path $resolved 'bin\windeployqt.exe'
    if(-not (Test-Path -LiteralPath $qmake) -or
       -not (Test-Path -LiteralPath $deploy)){ continue }
    $version=(& $qmake -query QT_VERSION 2>$null | Out-String).Trim()
    $spec=(& $qmake -query QMAKE_XSPEC 2>$null | Out-String).Trim()
    if($version -eq $requiredQtVersion -and $spec -match 'msvc'){
      return $resolved
    }
  }
  throw "Qt $requiredQtVersion MSVC 64-bit was not found. Put qmake in PATH, set QTDIR, or pass -QtRoot."
}

$vsRoot=Resolve-VisualStudioRoot $VisualStudioRoot
$vsDevCmd=Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
$cmake=Resolve-CMakeExecutable $CMakePath $vsRoot
$ctest=Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if(-not (Test-Path -LiteralPath $ctest)){
  $ctest=Resolve-CommandPath 'ctest.exe'
}
$qtRoot=Resolve-QtRoot $QtRoot
$qtBin=Join-Path $qtRoot 'bin'
$windeployqt=Join-Path $qtBin 'windeployqt.exe'
$resolvedBuildRoot=if([string]::IsNullOrWhiteSpace($BuildRoot)){
  Join-Path $root 'build'
} else {
  Resolve-InputPath $BuildRoot $root
}
$x86Build=Join-Path $resolvedBuildRoot 'nmake-x86'
$x64Build=Join-Path $resolvedBuildRoot 'nmake-x64'
$x86Output=Join-Path $x86Build $Config
$x64Output=Join-Path $x64Build $Config
$qtExecutableName='CH_Diagnostic_Studio.exe'
$dist=if([string]::IsNullOrWhiteSpace($DistPath)){
  Join-Path $root 'dist'
} else {
  Resolve-InputPath $DistPath $root
}
$redistRoot=Join-Path $vsRoot 'VC\Redist\MSVC'
if(-not (Test-Path -LiteralPath $redistRoot -PathType Container)){
  throw "MSVC redistributable directory was not found under: $vsRoot"
}
$crtDirectory=Get-ChildItem -LiteralPath $redistRoot -Directory |
  Sort-Object Name -Descending |
  ForEach-Object { Join-Path $_.FullName 'x64\Microsoft.VC143.CRT' } |
  Where-Object { Test-Path -LiteralPath $_ } |
  Select-Object -First 1

foreach($required in @($vsDevCmd,$cmake,$ctest,$windeployqt)){
  if(-not (Test-Path -LiteralPath $required)){ throw "Required build tool not found: $required" }
}
if(-not $crtDirectory){ throw 'MSVC x64 redistributable DLL directory was not found' }

$resolvedRoot=[IO.Path]::GetFullPath($root).TrimEnd('\')
$resolvedBuildRoot=[IO.Path]::GetFullPath($resolvedBuildRoot).TrimEnd('\')
$resolvedDist=[IO.Path]::GetFullPath($dist).TrimEnd('\')
foreach($scopedPath in @($resolvedBuildRoot,$resolvedDist)){
  if($scopedPath -eq $resolvedRoot -or
     -not $scopedPath.StartsWith($resolvedRoot + '\',
                                [StringComparison]::OrdinalIgnoreCase)){
    throw "Build and dist paths must remain under the repository: $scopedPath"
  }
}

Write-Host "Repository : $resolvedRoot"
Write-Host "Configuration: $Config"
Write-Host "Visual Studio: $vsRoot"
Write-Host "CMake     : $cmake"
Write-Host "Qt         : $qtRoot"
Write-Host "Build root : $resolvedBuildRoot"
Write-Host "Dist       : $resolvedDist"
if($ValidateEnvironmentOnly){
  Write-Host 'Build environment validation: PASS'
  return
}

function Copy-MsvcRuntime([string]$Destination) {
  Get-ChildItem -LiteralPath $crtDirectory -Filter '*.dll' -File |
    ForEach-Object {
      Copy-Item -LiteralPath $_.FullName -Destination $Destination -Force
    }
}

function Invoke-VsCommand([ValidateSet('x86','x64')][string]$Arch,
                          [string]$Command,
                          [string]$Description) {
  $wrapped='"{0}" -no_logo -arch={1} -host_arch=x64 >nul && {2}' -f $vsDevCmd,$Arch,$Command
  & cmd.exe /d /s /c $wrapped
  if($LASTEXITCODE -ne 0){
    throw "$Description failed with exit code $LASTEXITCODE"
  }
}

function Reset-StaleCMakeCache([string]$BuildDirectory) {
  $resolvedBuild=[IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\')
  if($resolvedBuild -eq $resolvedBuildRoot -or
     -not $resolvedBuild.StartsWith($resolvedBuildRoot + '\',
                                   [StringComparison]::OrdinalIgnoreCase)){
    throw "Refusing to inspect a build directory outside BuildRoot: $resolvedBuild"
  }
  $cache=Join-Path $resolvedBuild 'CMakeCache.txt'
  if(-not (Test-Path -LiteralPath $cache)){ return }
  $cacheLines=Get-Content -LiteralPath $cache
  $homeLine=$cacheLines | Where-Object {
    $_ -like 'CMAKE_HOME_DIRECTORY:INTERNAL=*'
  } | Select-Object -First 1
  $generatorLine=$cacheLines | Where-Object {
    $_ -like 'CMAKE_GENERATOR:INTERNAL=*'
  } | Select-Object -First 1
  $cachedHome=if($homeLine){ ($homeLine -split '=',2)[1] } else { '' }
  $cachedGenerator=if($generatorLine){
    ($generatorLine -split '=',2)[1]
  } else { '' }
  $normalizedHome=if($cachedHome){
    [IO.Path]::GetFullPath($cachedHome).TrimEnd('\')
  } else { '' }
  if($normalizedHome -eq $resolvedRoot -and
     $cachedGenerator -eq 'NMake Makefiles'){
    return
  }

  Write-Host "Refreshing stale CMake cache: $resolvedBuild"
  Remove-Item -LiteralPath $cache -Force
  $cmakeFiles=Join-Path $resolvedBuild 'CMakeFiles'
  if(Test-Path -LiteralPath $cmakeFiles){
    Remove-Item -LiteralPath $cmakeFiles -Recurse -Force
  }
  foreach($generatedFile in @('Makefile','cmake_install.cmake')){
    $path=Join-Path $resolvedBuild $generatedFile
    if(Test-Path -LiteralPath $path){
      Remove-Item -LiteralPath $path -Force
    }
  }
}

Reset-StaleCMakeCache $x86Build
Reset-StaleCMakeCache $x64Build

$x86Configure='"{0}" -S "{1}" -B "{2}" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE={3} -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="{4}" -DUDS_BUILD_QT_UI=OFF' -f $cmake,$root,$x86Build,$Config,$x86Output
Invoke-VsCommand x86 $x86Configure 'CMake x86 configure'
$x86Compile='"{0}" --build "{1}" --target keygen_broker' -f $cmake,$x86Build
Invoke-VsCommand x86 $x86Compile 'CMake x86 broker build'

$broker=Join-Path $x86Output 'keygen_broker.exe'
$x64Configure='"{0}" -S "{1}" -B "{2}" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE={3} -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="{4}" -DCMAKE_PREFIX_PATH="{5}" -DUDS_X86_KEYGEN_BROKER="{6}"' -f $cmake,$root,$x64Build,$Config,$x64Output,$qtRoot,$broker
Invoke-VsCommand x64 $x64Configure 'CMake x64 configure'
$x64Compile='"{0}" --build "{1}"' -f $cmake,$x64Build
Invoke-VsCommand x64 $x64Compile 'CMake x64 build'

$buildQtExecutable=Join-Path $x64Output $qtExecutableName
if(-not (Test-Path -LiteralPath $buildQtExecutable)){
  throw 'Qt target was not built; verify the Qt 5.15.2 CMake package'
}
$buildDeploy='"{0}" --release --no-translations --compiler-runtime "{1}"' -f $windeployqt,$buildQtExecutable
Invoke-VsCommand x64 $buildDeploy 'build-output windeployqt'
Copy-MsvcRuntime $x64Output

# Profile files are runtime inputs rather than compiled sources.  A no-op
# incremental build does not rerun the executable target's POST_BUILD copy, so
# keep the test runtime tree in sync explicitly before CTest.
$buildProfiles=Join-Path $x64Output 'profiles'
& $cmake -E remove_directory $buildProfiles
if($LASTEXITCODE -ne 0){ throw 'Failed to clear build-output profiles' }
& $cmake -E copy_directory (Join-Path $root 'profiles') $buildProfiles
if($LASTEXITCODE -ne 0){ throw 'Failed to synchronize build-output profiles' }

$testCommand='"{0}" --test-dir "{1}" --output-on-failure' -f $ctest,$x64Build
Invoke-VsCommand x64 $testCommand 'C++ tests'

if($resolvedDist -eq $resolvedRoot -or
   -not $resolvedDist.StartsWith($resolvedRoot + '\',
                                [StringComparison]::OrdinalIgnoreCase)){
  throw "Refusing to recreate dist outside the project: $resolvedDist"
}
if(Test-Path -LiteralPath $resolvedDist){
  & $cmake -E remove_directory $resolvedDist
  if($LASTEXITCODE -ne 0 -and
     (Get-ChildItem -LiteralPath $resolvedDist -Force | Select-Object -First 1)){
    throw "Failed to clear dist: $resolvedDist"
  }
}
& $cmake -E make_directory $resolvedDist
if($LASTEXITCODE -ne 0){ throw "Failed to create dist: $resolvedDist" }

$installCommand='"{0}" --install "{1}" --prefix "{2}"' -f $cmake,$x64Build,$dist
Invoke-VsCommand x64 $installCommand 'CMake install'
Copy-Item -LiteralPath $broker -Destination (Join-Path $dist 'keygen_broker.exe') -Force

# Keep the retired LP 4-byte DLL in the LP resource units only.  The source
# checkout may still retain an archival copy, but no generated ChuNeng runtime
# tree may contain two incompatible SeedKey providers.
foreach($location in @($x64Output,$dist)){
  $retiredChunengDll=Join-Path $location `
    'resources\chuneng_d7_arc331_zip\dll\66272f124ced1_lingpao_SeednKey_cdd.dll'
  if(Test-Path -LiteralPath $retiredChunengDll){
    & $cmake -E remove $retiredChunengDll
    if($LASTEXITCODE -ne 0){
      throw "Failed to remove retired Chuneng SeedKey DLL: $retiredChunengDll"
    }
  }
}

$qtExecutable=Join-Path $dist $qtExecutableName
if(-not (Test-Path -LiteralPath $qtExecutable)){
  throw 'Qt target was not built; verify the Qt 5.15.2 CMake package'
}
$distDeploy='"{0}" --release --no-translations --compiler-runtime "{1}"' -f $windeployqt,$qtExecutable
Invoke-VsCommand x64 $distDeploy 'dist windeployqt'
Copy-MsvcRuntime $dist

function Test-ProfileResources([string]$Location) {
  $profileDirectory=Join-Path $Location 'profiles'
  foreach($profile in Get-ChildItem -LiteralPath $profileDirectory `
                              -File -Filter '*.ini'){
    foreach($line in Get-Content -LiteralPath $profile.FullName -Encoding utf8){
      if($line -match `
          '^(driver_file|app_file|cal_file|driver_verify_file|app_verify_file|cal_verify_file|security_dll)=(.*)$'){
        $relativePath=$Matches[2].Trim()
        if($relativePath -and
           -not (Test-Path -LiteralPath (Join-Path $Location $relativePath))){
          throw "Profile resource is missing: $($profile.Name): $relativePath"
        }
      }
    }
  }
}

Test-ProfileResources $x64Output
Test-ProfileResources $dist

function Test-LongmaKeygen([string]$Location) {
  $broker=Join-Path $Location 'keygen_broker.exe'
  $longmaDll=Join-Path $Location 'resources\longma_ars1_31\dll\S202_SeednKey_cdd .dll'
  $longmaKey1=(& $broker $longmaDll '6BBD653A' '0x1' '' | Out-String).Trim()
  if($LASTEXITCODE -ne 0 -or $longmaKey1 -ne 'B75C2512'){
    throw "Longma ARS1.31 keygen vector 1 failed at ${Location}: $longmaKey1"
  }
  $longmaKey2=(& $broker $longmaDll '616DDCDF' '0x1' '' | Out-String).Trim()
  if($LASTEXITCODE -ne 0 -or $longmaKey2 -ne '1B98749D'){
    throw "Longma ARS1.31 keygen vector 2 failed at ${Location}: $longmaKey2"
  }
}
function Test-XizhongKeygen([string]$Location) {
  $broker=Join-Path $Location 'keygen_broker.exe'
  $vectors=@(
    @('RSMR','resources\xizhong_rsmr\XZ_GenerateKeyEx_RSMR.dll','29984258'),
    @('LSMR','resources\xizhong_lsmr\XZ_GenerateKeyEx_LSMR.dll','2A984258')
  )
  foreach($vector in $vectors){
    $xizhongDll=Join-Path $Location $vector[1]
    $key=(& $broker $xizhongDll 'FDBAAF18' '0x11' '' | Out-String).Trim()
    if($LASTEXITCODE -ne 0 -or $key -ne $vector[2]){
      throw "Xizhong $($vector[0]) keygen vector failed at ${Location}: $key"
    }
  }
}
function Test-C857Keygen([string]$Location) {
  $broker=Join-Path $Location 'keygen_broker.exe'
  foreach($projectDirectory in @('changan_c857','lingyao_b216')){
  $slaveDll=Join-Path $Location "resources\$projectDirectory\dll\SeedKey_Slave.dll"
  $slaveKey1=(& $broker $slaveDll 'E8D02A04' '0x1' '' |
    Out-String).Trim()
  if($LASTEXITCODE -ne 0 -or $slaveKey1 -ne 'CB34CAAB'){
    throw "$projectDirectory slave keygen vector 1 failed at ${Location}: $slaveKey1"
  }
  $slaveKey2=(& $broker $slaveDll 'D8CA282C' '0x1' '' |
    Out-String).Trim()
  if($LASTEXITCODE -ne 0 -or $slaveKey2 -ne '0EDE84B7'){
    throw "$projectDirectory slave keygen vector 2 failed at ${Location}: $slaveKey2"
  }
  $mainDll=Join-Path $Location "resources\$projectDirectory\dll\SeedKey_Main.dll"
  $mainKey=(& $broker $mainDll 'E8D02A04' '0x1' '' |
    Out-String).Trim()
  if($LASTEXITCODE -ne 0 -or $mainKey -ne 'EF394253'){
    throw "$projectDirectory main keygen load check failed at ${Location}: $mainKey"
  }
  }
}
function Test-ChunengKeygen([string]$Location) {
  $broker=Join-Path $Location 'keygen_broker.exe'
  $chunengDll=Join-Path $Location `
    'resources\chuneng_d7_arc331_zip\dll\ChuNeng_D7_SeednKey_V1.0.dll'
  $key=(& $broker $chunengDll `
    '2110F60B7F456E9A670FE43D94A86E0C' '0x11' 'chuneng' |
    Out-String).Trim()
  if($LASTEXITCODE -ne 0 -or
     $key -ne 'FFD1FC2E7DC44F24279B1A5EAAFF21D7'){
    throw "Chuneng 331 keygen vector failed at ${Location}: $key"
  }
}
function Test-ChunengInputSet([string]$Location) {
  $root=Join-Path $Location 'resources\chuneng_d7_arc331_zip'
  $driver=Join-Path $root 'CBF\Driver\driver_712345678AB.cbf'
  $app=Join-Path $root 'CBF\APP\7052A5023002AB.cbf'
  $expected=[ordered]@{}
  $expected[$driver]='A3D4B9A5323FDA405400712FA5E46D8CD0CB1D9AD218AEBCC35008716FB933C3'
  $expected[$app]='3EEF5C26084570BD9B1E7C5430025A2A5ED307AEE955793BCC35CC05C8278205'
  $expected[(Join-Path $root 'S19\Driver.s19')]='627C52ECF809D46176B8ACB73CB62BB5FB8F05FD8AFCB5BA475B8B7AD40D0DA9'
  $expected[(Join-Path $root 'S19\Driver_Ver.asc')]='43CD2F1031F16B84D0CAD8685615C63B52C7D5AFA4A432E64B943383E877FE54'
  $expected[(Join-Path $root 'S19\Driver_ABT.asc')]='CFA6DC80812B542050037D26059A0E5D632C666328270E690C7C048D3671E091'
  $expected[(Join-Path $root 'S19\APP.s19')]='1492CAFECEE8715F23DCF0E4E5C1B549C4414DFCB27BB02DE7A123EC06AE1AAE'
  $expected[(Join-Path $root 'S19\APP_Ver.asc')]='65E439803EC7A2E66A059F8CF60A3ADC0150EAFD8F73EB09D671FB317D281F89'
  $expected[(Join-Path $root 'S19\APP_ABT.asc')]='DC8AA6C4CBEC64AD0553F98EC7B812E04839C6D7353CC11A0D20D563D3673F69'
  foreach($path in $expected.Keys){
    if(-not (Test-Path -LiteralPath $path)){
      throw "ChuNeng ARC331 active input is missing: $path"
    }
    $actual=Get-Sha256Hex $path
    if($actual -ne $expected[$path]){
      throw "ChuNeng ARC331 active input hash mismatch: $path"
    }
  }
}
function Test-CheryKp31Keygen([string]$Location) {
  $broker=Join-Path $Location 'keygen_broker.exe'
  foreach($project in @('chery_kp31','chery_e0y')){
    $dll=Join-Path $Location `
      "resources\$project\dll\CHERY_E0Y_UPDATE23231115.dll"
    $key=(& $broker $dll `
      '00000000000000000000000000000000' '0x11' '' |
      Out-String).Trim()
    if($LASTEXITCODE -ne 0 -or
       $key -ne 'EB458ED62435F7ED59C0C032D16E9EDC'){
      throw "Chery $project level 0x11 keygen zero vector failed at ${Location}: $key"
    }
  }
  foreach($project in @('chery_t1ej','chery_t22')){
    $dll=Join-Path $Location `
      "resources\$project\dll\CIR_GenerateKeyEx.dll"
    $key=(& $broker $dll '00000000' '0x07' '' |
      Out-String).Trim()
    if($LASTEXITCODE -ne 0 -or $key -ne 'FFFF93BC'){
      throw "Chery $project GenerateKeyExOpt level 0x07 vector failed at ${Location}: $key"
    }
  }
}
function Test-ShidaixinanKeygen([string]$Location) {
  $broker=Join-Path $Location 'keygen_broker.exe'
  foreach($resourceDirectory in @(
    'shidaixinan_hjzj_fmr',
    'shidaixinan_arf232_common')){
    $fmrDll=Join-Path $Location "resources\$resourceDirectory\dll\FMR.dll"
    $level1=(& $broker $fmrDll '7A45FA55' '0x1' '' |
      Out-String).Trim()
    if($LASTEXITCODE -ne 0 -or $level1 -ne 'B016E13B'){
      throw "Shidaixinan $resourceDirectory level 1 keygen vector failed at ${Location}: $level1"
    }
    $level3=(& $broker $fmrDll 'E93FDFD0' '0x3' '' |
      Out-String).Trim()
    if($LASTEXITCODE -ne 0 -or $level3 -ne '807AA7FA'){
      throw "Shidaixinan $resourceDirectory level 3 keygen vector failed at ${Location}: $level3"
    }
  }
}
function Test-LingpaoRadarKeygen([string]$Location) {
  $broker=Join-Path $Location 'keygen_broker.exe'
  foreach($relativeDll in @(
    'resources\lp_arc\dll\66272f124ced1_lingpao_SeednKey_cdd.dll',
    'resources\lp_arf\dll\66272f124ced1_lingpao_SeednKey_cdd.dll',
    'resources\lp_arf231_a12\dll\ARF2.31CC3_LPA12P_SeednKey_cdd.dll',
    'resources\lp_arf231_b11\dll\lingpao_SeednKey_cdd.dll')){
    $dll=Join-Path $Location $relativeDll
    $key1=(& $broker $dll 'FFFD13DE' '0x11' 'lingpao' |
      Out-String).Trim()
    if($LASTEXITCODE -ne 0 -or $key1 -ne 'C0828573'){
      throw "$relativeDll keygen vector 1 failed at ${Location}: $key1"
    }
    $key2=(& $broker $dll 'FFFD03D0' '0x11' 'lingpao' |
      Out-String).Trim()
    if($LASTEXITCODE -ne 0 -or $key2 -ne '1407370F'){
      throw "$relativeDll keygen vector 2 failed at ${Location}: $key2"
    }
  }
}
Test-LongmaKeygen $x64Output
Test-XizhongKeygen $x64Output
Test-C857Keygen $x64Output
Test-ChunengKeygen $x64Output
Test-ChunengInputSet $x64Output
Test-CheryKp31Keygen $x64Output
Test-ShidaixinanKeygen $x64Output
Test-LingpaoRadarKeygen $x64Output
Test-LongmaKeygen $dist
Test-XizhongKeygen $dist
Test-C857Keygen $dist
Test-ChunengKeygen $dist
Test-ChunengInputSet $dist
Test-CheryKp31Keygen $dist
Test-ShidaixinanKeygen $dist
Test-LingpaoRadarKeygen $dist
Write-Host 'Longma ARS1.31 keygen vectors in build output and dist: PASS'
Write-Host 'Xizhong RSMR/LSMR keygen vectors in build output and dist: PASS'
Write-Host 'Changan C857 and B216 independent main/slave keygen resources: PASS'
Write-Host 'Chuneng 331 keygen vector in build output and selected dist: PASS'
Write-Host 'Chuneng ARC331 paired Driver + APP CBF input hashes: PASS'
Write-Host 'Chery KP31/E0Y level 0x11 and T1EJ/T22 GenerateKeyExOpt level 0x07 vectors: PASS'
Write-Host 'Shidaixinan HJZJ_FMR level 1/3 keygen vectors in build output and dist: PASS'
Write-Host 'LP-ARC/LP-ARF/A12/B11 level 11 keygen vectors in build output and dist: PASS'
Write-Host "Built: $dist"
