param([string]$InstallScript, [string]$PackageDirectory)
$ErrorActionPreference='Stop'
$testRoot=Join-Path ([IO.Path]::GetTempPath()) ('r-ime-install-test-'+[Guid]::NewGuid().ToString('N'))
$state=Join-Path $testRoot 'R-IME'
New-Item -ItemType Directory -Path $state | Out-Null
try {
    $manifest=Join-Path $state 'installation.json'
    $original=@{status='installed';version='0.2.0-20000101000000';root=(Join-Path $testRoot 'Programs');directory=(Join-Path $testRoot 'Programs/0.2.0-20000101000000')} | ConvertTo-Json
    Set-Content -LiteralPath $manifest -Value $original
    $runner=Join-Path $testRoot 'simulate-denied-elevation.ps1'
    @'
param($TestRoot,$InstallScript,$PackageDirectory)
$env:LOCALAPPDATA=$TestRoot
function Start-Process {
    $failed=[pscustomobject]@{ExitCode=42}
    $failed | Add-Member ScriptMethod WaitForExit {}
    return $failed
}
& $InstallScript -SourceDirectory $PackageDirectory -InstallRoot (Join-Path $TestRoot 'Programs') -Version '0.2.0-20000102000000'
'@ | Set-Content -LiteralPath $runner
    1..2 | ForEach-Object {
        & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File $runner $testRoot $InstallScript $PackageDirectory *> (Join-Path $testRoot 'failure.log')
        if($LASTEXITCODE -eq 0){throw 'Simulated system failure unexpectedly succeeded'}
        if((Get-Content -LiteralPath $manifest -Raw).Trim() -ne $original.Trim()){throw 'Failed upgrade replaced the successful installation manifest'}
        $attempt=Get-Content (Join-Path $state 'installation-attempt.json') -Raw | ConvertFrom-Json
        if($attempt.status -ne 'failed' -or $attempt.error -notmatch 'exit 42'){throw 'Failed attempt was not retained with its error'}
        if((Get-Content (Join-Path $state 'previous-installation.json') -Raw).Trim() -ne $original.Trim()){throw 'Repeated failure lost the last successful version'}
    }
    'Installer failure recovery: successful version retained after two denied system phases'
} finally {
    $resolved=[IO.Path]::GetFullPath($testRoot)
    $temp=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
    if(!$resolved.StartsWith($temp,[StringComparison]::OrdinalIgnoreCase) -or (Split-Path $resolved -Leaf) -notlike 'r-ime-install-test-*'){throw 'Unsafe test cleanup path'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
