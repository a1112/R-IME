param([switch]$SystemPhase,[string]$Directory='', [string]$InstallRoot='')
$ErrorActionPreference='Stop'
if($SystemPhase){
    $root=[IO.Path]::GetFullPath($InstallRoot);$directory=[IO.Path]::GetFullPath($Directory)
    if(!$directory.StartsWith($root+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or (Split-Path $directory -Leaf) -notmatch '^0\.2\.0-\d{14}$'){throw 'Unsafe installation directory'}
    foreach($tool in @('x86/r-ime-register.exe','r-ime-register.exe')){
        $utility=Join-Path $directory $tool
        if(Test-Path -LiteralPath $utility){& $utility --unregister;if($LASTEXITCODE){throw 'Unregister failed; files retained'}}
    }
    # A loaded TSF DLL may remain; never terminate applications to force removal.
    if(Test-Path -LiteralPath $directory){try{Remove-Item -LiteralPath $directory -Recurse -Force}catch{Write-Warning "Loaded program files remain in $directory; remove them after closing applications."}}
    exit 0
}
$manifest=Join-Path $env:LOCALAPPDATA 'R-IME/installation.json'
if(!(Test-Path -LiteralPath $manifest)){$manifest=Join-Path $env:LOCALAPPDATA 'R-IME/installation-attempt.json'}
if(!(Test-Path -LiteralPath $manifest)){throw 'No R-IME installation or incomplete installation found'}
$installation=Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
$directory=[IO.Path]::GetFullPath($installation.directory);$root=[IO.Path]::GetFullPath($installation.root)
if(!$directory.StartsWith($root+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Installation escaped its root'}
$engine=Join-Path $directory 'r-ime-engine.exe';$utility=Join-Path $directory 'r-ime-register.exe'
if(Test-Path -LiteralPath $engine){Start-Process -FilePath $engine -ArgumentList '--shutdown' -WindowStyle Hidden -Wait}
if(Test-Path -LiteralPath $utility){& $utility --disable-user;if($LASTEXITCODE){throw 'Removing the current-user R-IME input entry failed'}}
$argsForSystem=@('-NoProfile','-File',('"'+$PSCommandPath+'"'),'-SystemPhase','-Directory',('"'+$directory+'"'),'-InstallRoot',('"'+$root+'"'))
$process=Start-Process -FilePath (Join-Path $PSHOME 'pwsh.exe') -Verb RunAs -ArgumentList $argsForSystem -WindowStyle Hidden -PassThru
$process.WaitForExit();if($process.ExitCode){throw 'System unregistration failed; user preferences retained'}
$languages=Get-WinUserLanguageList
foreach($language in $languages){[void]$language.InputMethodTips.Remove([string]$installation.tip)}
Set-WinUserLanguageList $languages -Force
Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'R-IME' -ErrorAction SilentlyContinue
$shortcut=Join-Path ([Environment]::GetFolderPath('Programs')) 'R-IME/R-IME 设置.lnk'
if(Test-Path -LiteralPath $shortcut){Remove-Item -LiteralPath $shortcut}
Remove-Item -LiteralPath $manifest
Write-Output "R-IME unregistered. User dictionaries and settings remain in $env:LOCALAPPDATA\R-IME"
