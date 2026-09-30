param(
    [Parameter(Mandatory=$true)][string]$QtRoot,
    [string]$CMake = 'cmake',
    [string]$BuildDirectory = '',
    [string]$X86BuildDirectory = '',
    [switch]$SkipBuild
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if(!$BuildDirectory){$BuildDirectory=Join-Path $repo 'build-windows'}
if(!$X86BuildDirectory){$X86BuildDirectory=Join-Path $repo 'build-windows-x86'}
$dependencies=Join-Path $repo '.cache/windows-deps'
New-Item -ItemType Directory -Force $dependencies | Out-Null
$archive=Join-Path $dependencies 'rime.7z'
$digest='7478C7CAA4FF6B37DE86DABA1F7CE4A994A4F5BA24872A820FB2B3A9B01FED15'
if(!(Test-Path $archive)){Invoke-WebRequest 'https://github.com/rime/librime/releases/download/1.17.0/rime-33e7814-Windows-msvc-x64.7z' -OutFile $archive}
if((Get-FileHash $archive -Algorithm SHA256).Hash -ne $digest){throw 'librime archive SHA256 mismatch'}
if(!(Test-Path (Join-Path $dependencies 'dist/lib/rime.dll'))){& tar -xf $archive -C $dependencies;if($LASTEXITCODE){throw 'Cannot extract librime'}}
function Invoke-Checked([string]$Executable,[string[]]$Arguments){& $Executable @Arguments;if($LASTEXITCODE){throw "$Executable failed with exit code $LASTEXITCODE"}}
if(!$SkipBuild){
    Invoke-Checked $CMake @('-S',$repo,'-B',$BuildDirectory,'-G','Visual Studio 17 2022','-A','x64',"-DCMAKE_PREFIX_PATH=$QtRoot","-DRIME_SDK_ROOT=$dependencies/dist",'-DRIME_BUILD_WINDOWS_TSF=ON')
    Invoke-Checked $CMake @('--build',$BuildDirectory,'--config','Release','-j','4')
    Invoke-Checked $CMake @('-S',$repo,'-B',$X86BuildDirectory,'-G','Visual Studio 17 2022','-A','Win32','-DRIME_BUILD_ENGINE=OFF','-DRIME_BUILD_SETTINGS=OFF','-DRIME_BUILD_WINDOWS_TSF=ON')
    Invoke-Checked $CMake @('--build',$X86BuildDirectory,'--config','Release','--target','r-ime-tsf','r-ime-register','-j','4')
}
$payload=Join-Path $BuildDirectory 'package'
New-Item -ItemType Directory -Force $payload,(Join-Path $payload 'x86'),(Join-Path $payload 'share/rime-data'),(Join-Path $payload 'share/r-ime/rime') | Out-Null
foreach($file in @('r-ime-engine.exe','rime.dll','r-ime-settings.exe','r-ime-tsf.dll','r-ime-register.exe')){Copy-Item -LiteralPath (Join-Path $BuildDirectory "bin/$file") -Destination $payload -Force}
foreach($file in @('r-ime-tsf.dll','r-ime-register.exe')){Copy-Item -LiteralPath (Join-Path $X86BuildDirectory "bin/$file") -Destination (Join-Path $payload 'x86') -Force}
Copy-Item (Join-Path $repo 'data/rime/*') (Join-Path $payload 'share/r-ime/rime') -Force
Invoke-Checked 'tar' @('-xf',(Join-Path $PSScriptRoot 'rime-data.tar.gz'),'-C',(Join-Path $payload 'share/rime-data'))
New-Item -ItemType Directory -Force (Join-Path $BuildDirectory 'bin/share') | Out-Null
Copy-Item (Join-Path $payload 'share/*') (Join-Path $BuildDirectory 'bin/share') -Recurse -Force
Invoke-Checked (Join-Path $QtRoot 'bin/windeployqt.exe') @('--release','--verbose','0','--no-translations','--no-system-d3d-compiler','--qmldir',(Join-Path $repo 'src/settings'),'--dir',$payload,(Join-Path $payload 'r-ime-settings.exe'))
# Ship the redistributable CRT app-locally for both bitnesses; no machine-wide runtime change.
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
$redist=Get-ChildItem (Join-Path $vs 'VC/Redist/MSVC') -Directory | Where-Object Name -Match '^\d' | Sort-Object Name -Descending | Select-Object -First 1
foreach($arch in @('x64','x86')){
    $destination=if($arch -eq 'x64'){$payload}else{Join-Path $payload 'x86'}
    Copy-Item (Join-Path $redist.FullName "$arch/Microsoft.VC143.CRT/*.dll") $destination -Force
}
Copy-Item (Join-Path $PSScriptRoot 'licenses') $payload -Recurse -Force
New-Item -ItemType Directory -Force (Join-Path $payload 'licenses/qt') | Out-Null
Get-ChildItem (Join-Path $QtRoot '../../Licenses') -File -ErrorAction SilentlyContinue | Copy-Item -Destination (Join-Path $payload 'licenses/qt') -Force
Copy-Item (Join-Path $PSScriptRoot 'THIRD-PARTY.md') $payload -Force
Copy-Item -LiteralPath (Join-Path $repo 'docs/windows.md') -Destination (Join-Path $payload 'README.md') -Force
Copy-Item (Join-Path $PSScriptRoot 'install.ps1'),(Join-Path $PSScriptRoot 'uninstall.ps1') $payload -Force
Write-Output "Windows package: $payload"
