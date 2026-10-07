param(
    [string]$SourceDirectory=$PSScriptRoot,
    [string]$InstallRoot=(Join-Path $env:ProgramFiles 'R-IME'),
    [string]$Version=('0.2.0-'+(Get-Date -Format 'yyyyMMddHHmmss')),
    [switch]$SystemPhase
)
$ErrorActionPreference='Stop'
$source=(Resolve-Path -LiteralPath $SourceDirectory).Path
$root=[IO.Path]::GetFullPath($InstallRoot)
if($Version -notmatch '^0\.2\.0-\d{14}$'){throw 'Invalid installation version directory'}
$destination=Join-Path $root $Version
$tip='0804:{1A5D0370-40A3-4865-90A6-324E1BB1855B}{80B722F7-FA50-4D8E-A084-E12A5B906585}'
$state=Join-Path $env:LOCALAPPDATA 'R-IME'
$manifest=Join-Path $state 'installation.json'
$attempt=Join-Path $state 'installation-attempt.json'
foreach($file in @('r-ime-engine.exe','r-ime-settings.exe','r-ime-tsf.dll','r-ime-register.exe','r-ime-ctl.exe','x86/r-ime-tsf.dll','x86/r-ime-register.exe')){if(!(Test-Path -LiteralPath (Join-Path $source $file))){throw "Incomplete package: $file"}}
if($SystemPhase){
    # Elevation is limited to protected binaries and machine TSF/COM registration.
    # Never launch the worker or settings from the elevated phase.
    New-Item -ItemType Directory -Force $destination | Out-Null
    Copy-Item (Join-Path $source '*') $destination -Recurse -Force
    foreach($arch in @('','x86/')){
        & (Join-Path $destination ($arch+'r-ime-register.exe')) --register (Join-Path $destination ($arch+'r-ime-tsf.dll'))
        if($LASTEXITCODE){throw "TSF registration failed: $arch"}
        & (Join-Path $destination ($arch+'r-ime-register.exe')) --check
        if($LASTEXITCODE){throw "Registered COM/TSF verification failed: $arch"}
    }
    exit 0
}
New-Item -ItemType Directory -Force $state | Out-Null
$previous=if(Test-Path -LiteralPath $manifest){Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json}else{$null}
if($previous -and $previous.status -eq 'installed'){Copy-Item -LiteralPath $manifest -Destination (Join-Path $state 'previous-installation.json') -Force}
$record=@{version=$Version;root=$root;directory=$destination;tip=$tip;status='pending'}
$record | ConvertTo-Json | Set-Content -LiteralPath $attempt -Encoding utf8
$languageBefore=Get-WinUserLanguageList
$defaultBefore=Get-WinDefaultInputMethodOverride
$languageBefore | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $state 'language-list-before.json') -Encoding utf8
try {
    $arguments=@('-NoProfile','-File',('"'+$PSCommandPath+'"'),'-SourceDirectory',('"'+$source+'"'),'-InstallRoot',('"'+$root+'"'),'-Version',$Version,'-SystemPhase')
    $process=Start-Process -FilePath (Join-Path $PSHOME 'pwsh.exe') -Verb RunAs -ArgumentList $arguments -WindowStyle Hidden -PassThru
    $process.WaitForExit()
    if($process.ExitCode){throw "System registration phase failed (exit $($process.ExitCode)); staged files and uninstall information are retained"}
    & (Join-Path $destination 'r-ime-register.exe') --enable-user
    if($LASTEXITCODE){throw 'Enabling R-IME in the current-user input list failed'}
    foreach($arch in @('','x86/')){& (Join-Path $destination ($arch+'r-ime-register.exe')) --check;if($LASTEXITCODE){throw "Current-user COM verification failed: $arch"}}
    $languages=Get-WinUserLanguageList;$chinese=$null
    foreach($language in $languages){if($language.LanguageTag -eq 'zh-Hans-CN'){$chinese=$language;break}}
    if(!$chinese){$extra=New-WinUserLanguageList 'zh-Hans-CN';$languages.Add($extra[0]);$chinese=$languages[$languages.Count-1]}
    if(!$chinese.InputMethodTips.Contains($tip)){$chinese.InputMethodTips.Add($tip);Set-WinUserLanguageList $languages -Force}
    if($defaultBefore -and $defaultBefore.InputMethodTip){Set-WinDefaultInputMethodOverride -InputTip $defaultBefore.InputMethodTip}
    $shell=New-Object -ComObject WScript.Shell
    $menu=Join-Path ([Environment]::GetFolderPath('Programs')) 'R-IME'
    New-Item -ItemType Directory -Force $menu | Out-Null
    $shortcut=$shell.CreateShortcut((Join-Path $menu 'R-IME 设置.lnk'));$shortcut.TargetPath=Join-Path $destination 'r-ime-settings.exe';$shortcut.WorkingDirectory=$destination;$shortcut.Save()
    $run='HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    if(!(Test-Path -LiteralPath $run)){New-Item -Path $run | Out-Null}
    New-ItemProperty -Path $run -Name 'R-IME' -Value ('"'+(Join-Path $destination 'r-ime-engine.exe')+'"') -PropertyType String -Force | Out-Null
    if($previous){
        $oldRoot=[IO.Path]::GetFullPath($previous.root);$oldEngine=Join-Path ([IO.Path]::GetFullPath($previous.directory)) 'r-ime-engine.exe'
        if($oldEngine.StartsWith($oldRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $oldEngine)){
            Start-Process -FilePath $oldEngine -ArgumentList '--shutdown' -WindowStyle Hidden -Wait
            $deadline=(Get-Date).AddSeconds(15)
            do {$running=@(Get-Process -Name r-ime-engine -ErrorAction SilentlyContinue | Where-Object Path -EQ $oldEngine);if(!$running){break};Start-Sleep -Milliseconds 100}while((Get-Date) -lt $deadline)
            if($running){throw 'The previous engine is still stopping; rerun installation after it exits'}
        }
    }
    Start-Process -FilePath (Join-Path $destination 'r-ime-engine.exe') -WorkingDirectory $destination -WindowStyle Hidden
    $ready=$false;$deadline=(Get-Date).AddSeconds(90)
    do {
        $probe=Start-Process -FilePath (Join-Path $destination 'r-ime-engine.exe') -ArgumentList '--status' -WindowStyle Hidden -PassThru -Wait
        if($probe.ExitCode -eq 0){$ready=$true;break}
        Start-Sleep -Milliseconds 300
    }while((Get-Date) -lt $deadline)
    if(!$ready){throw 'The installed engine did not become ready within 90 seconds'}
    $record.status='installed';$record.menu=$menu
    $record | ConvertTo-Json | Set-Content -LiteralPath $manifest -Encoding utf8
    Remove-Item -LiteralPath $attempt -ErrorAction SilentlyContinue
    Write-Output "R-IME installed: $destination"
} catch {
    $record.status='failed';$record.error=[string]$_;$record.location=$_.InvocationInfo.PositionMessage;$record | ConvertTo-Json | Set-Content -LiteralPath $attempt -Encoding utf8
    Write-Error "Installation did not finish at $($_.InvocationInfo.PositionMessage): $_"
    exit 1
}
