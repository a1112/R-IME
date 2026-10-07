param(
    [string]$InstalledDirectory='',
    [string]$DebuggerRoot=(Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/Debuggers'),
    [string]$BuildDirectory='',
    [string]$X86BuildDirectory=''
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if(!$InstalledDirectory){$InstalledDirectory=(Get-Content "$env:LOCALAPPDATA\R-IME\installation.json" -Raw | ConvertFrom-Json).directory}
if(!$BuildDirectory){$BuildDirectory=Join-Path $repo 'build-windows'}
if(!$X86BuildDirectory){$X86BuildDirectory=Join-Path $repo 'build-windows-x86'}
$results=Join-Path $repo ('.cache/installed-validation-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $results | Out-Null
foreach($arch in @('x64','x86')){
    $debugger=Join-Path $DebuggerRoot "$arch/cdb.exe"
    $build=if($arch -eq 'x64'){$BuildDirectory}else{$X86BuildDirectory}
    $test=Join-Path $build 'bin/windows-tsf-test.exe'
    if(!(Test-Path -LiteralPath $debugger) -or !(Test-Path -LiteralPath $test)){throw "Missing $arch test host or Windows SDK Debugging Tools"}
    foreach($mode in @('installed','installed-ui-less')){
    $result=Join-Path $results "$arch-$mode.json"
    # Windows refuses foreground activation from a background automation host.
    # Running our test document under the supported debugger grants the normal
    # documented foreground exception; no changes to other applications are made.
    # The readiness loop deliberately catches C++ exceptions until the private
    # worker is ready. Suppress first-chance reporting to avoid debugger pauses
    # distorting the production 40 ms IPC deadline. JSON remains authoritative.
    $arguments=@('-g','-G','-c','"sxd e06d7363; g"',('"'+$test+'"'),('--'+$mode),('"'+$InstalledDirectory+'"'),('"'+$result+'"'))
    $process=Start-Process -FilePath $debugger -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $results "$arch-$mode.log") -RedirectStandardError (Join-Path $results "$arch-$mode.stderr.log")
    if(!$process.WaitForExit(60000)){$process.Kill($true);throw "$arch installed TSF validation timed out"}
    if(!(Test-Path -LiteralPath $result)){throw "$arch test did not produce a result; inspect $results"}
    $outcome=Get-Content -LiteralPath $result -Raw | ConvertFrom-Json
    if($outcome.success -ne $true){throw "$arch installed TSF failed: $($outcome.message)"}
    Write-Output "$arch $mode TSF: $($outcome.message)"
    }
}
foreach($operation in @('status','configuration')){
    $ctl=Join-Path $InstalledDirectory 'r-ime-ctl.exe'
    if(!(Test-Path -LiteralPath $ctl)){throw 'Installed control CLI is missing'}
    $reply=if($operation -eq 'status'){& $ctl status}else{& $ctl config get}
    if($LASTEXITCODE){throw "Installed control CLI failed: $operation"}
    $control=$reply | ConvertFrom-Json
    if($control.ok -ne $true -or !$control.config){throw "Invalid control reply: $operation"}
    if($operation -eq 'configuration' -and $null -eq $control.phrases){throw 'Control configuration snapshot lacks phrases'}
    Write-Output "Installed control CLI: $operation passed"
}
Write-Output "Validation results: $results"
