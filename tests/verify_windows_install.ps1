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
    $result=Join-Path $results "$arch.json"
    # Windows refuses foreground activation from a background automation host.
    # Running our test document under the supported debugger grants the normal
    # documented foreground exception; no changes to other applications are made.
    $arguments=@('-g','-G',('"'+$test+'"'),'--installed',('"'+$InstalledDirectory+'"'),('"'+$result+'"'))
    $process=Start-Process -FilePath $debugger -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $results "$arch.log") -RedirectStandardError (Join-Path $results "$arch.stderr.log")
    if(!$process.WaitForExit(60000)){$process.Kill($true);throw "$arch installed TSF validation timed out"}
    if(!(Test-Path -LiteralPath $result)){throw "$arch test did not produce a result; inspect $results"}
    $outcome=Get-Content -LiteralPath $result -Raw | ConvertFrom-Json
    if($outcome.success -ne $true){throw "$arch installed TSF failed: $($outcome.message)"}
    Write-Output "$arch installed TSF: brand icons, visible on-screen candidates, real Windows key dispatch, preedit, Chinese commit and cancellation passed"
}
Write-Output "Validation results: $results"
