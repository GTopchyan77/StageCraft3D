<#
.SYNOPSIS
    Runs the StageCraft automation tests headless and checks that they left the user's data alone (STATE.md #55).

.DESCRIPTION
    Starts UnrealEditor-Cmd on the project with "Automation RunTests <Filter>", then reports:
      - how many tests were found (and fails when -ExpectedCount is given and differs: a test named X.Y.Z hides a test X.Y);
      - every test that did not succeed;
      - whether the user's data changed: the profile save slot, Saved/StageCraft, Saved/Config/WindowsEditor/GameUserSettings.ini
        and %LOCALAPPDATA%\StageCraft (credential vault, bridge session file). Integration tests run in temporary folders
        (FStageTestGameInstance), so any change is a bug.

    By default the tests run with an offscreen renderer, so the export, send and hand-off paths run for real. -NullRHI runs
    without one (faster; those deliveries are then asserted to fail as "no renderer").

    The full log is written to Saved\Automation\Logs\RunAutomation-<time>.log. Exit code 0 only when every test passed, the
    count matched (when given) and no user data changed. Build the editor target first.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File Scripts\Tests\RunAutomation.ps1
    powershell -ExecutionPolicy Bypass -File Scripts\Tests\RunAutomation.ps1 -Filter StageCraft.Economy -NullRHI
    powershell -ExecutionPolicy Bypass -File Scripts\Tests\RunAutomation.ps1 -ExpectedCount 150
#>
param(
    [string]$Filter = 'StageCraft',
    [int]$ExpectedCount = 0,
    [switch]$NullRHI,
    [int]$TimeoutSeconds = 1800
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Project = Get-ChildItem -Path $ProjectRoot -Filter *.uproject | Select-Object -First 1
if (-not $Project) { throw "No .uproject in $ProjectRoot" }

$Association = (Get-Content $Project.FullName -Raw | ConvertFrom-Json).EngineAssociation
$EngineRoot = (Get-ItemProperty -Path "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$Association" -Name InstalledDirectory -ErrorAction SilentlyContinue).InstalledDirectory
if (-not $EngineRoot) { $EngineRoot = "C:\Program Files\Epic Games\UE_$Association" }
$EditorCmd = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if (-not (Test-Path $EditorCmd)) { throw "UnrealEditor-Cmd not found at $EditorCmd" }

# --- What the tests must not change ---
$Watched = @(
    (Join-Path $ProjectRoot 'Saved\SaveGames'),
    (Join-Path $ProjectRoot 'Saved\StageCraft'),
    (Join-Path $ProjectRoot 'Saved\Config\WindowsEditor\GameUserSettings.ini'),
    (Join-Path $env:LOCALAPPDATA 'StageCraft')
)

function Get-Snapshot([string[]]$Paths) {
    $Snapshot = @{}
    foreach ($Path in $Paths) {
        if (-not (Test-Path $Path)) { continue }
        foreach ($File in Get-ChildItem -Path $Path -File -Recurse -Force -ErrorAction SilentlyContinue) {
            $Snapshot[$File.FullName] = (Get-FileHash -Path $File.FullName -Algorithm SHA256).Hash
        }
    }
    return $Snapshot
}

$Before = Get-Snapshot $Watched

# --- Run ---
$LogDir = Join-Path $ProjectRoot 'Saved\Automation\Logs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$LogFile = Join-Path $LogDir ("RunAutomation-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
$Arguments = @("`"$($Project.FullName)`"", "-ExecCmds=`"Automation RunTests $Filter; Quit`"", '-unattended', '-nosplash', '-stdout', '-FullStdOutLogOutput')
$Arguments += $(if ($NullRHI) { '-nullrhi' } else { '-RenderOffscreen' })

Write-Host "Running '$Filter' ($(if ($NullRHI) { 'no renderer' } else { 'offscreen renderer' }))..."
$Process = Start-Process -FilePath $EditorCmd -ArgumentList $Arguments -NoNewWindow -PassThru -RedirectStandardOutput $LogFile -RedirectStandardError "$LogFile.err"
if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
    Stop-Process -Id $Process.Id -Force
    throw "The tests did not finish within $TimeoutSeconds s; see $LogFile"
}

# --- Report ---
$Log = Get-Content $LogFile
$FoundLine = $Log | Select-String -Pattern 'Found (\d+) automation tests' | Select-Object -First 1
$Found = if ($FoundLine) { [int]$FoundLine.Matches[0].Groups[1].Value } else { 0 }
$Completed = $Log | Select-String -Pattern 'Test Completed\. Result=\{(\w+)\} Name=\{[^}]*\} Path=\{([^}]*)\}'
$Passed = @($Completed | Where-Object { $_.Matches[0].Groups[1].Value -eq 'Success' })
$Failed = @($Completed | Where-Object { $_.Matches[0].Groups[1].Value -ne 'Success' })
$ExitLine = $Log | Select-String -Pattern 'TEST COMPLETE\. EXIT CODE: (-?\d+)' | Select-Object -First 1

$Ok = $true
Write-Host ("Found {0} tests; {1} passed, {2} did not." -f $Found, $Passed.Count, $Failed.Count)
foreach ($Line in $Failed) {
    Write-Host ("  FAILED  {0} ({1})" -f $Line.Matches[0].Groups[2].Value, $Line.Matches[0].Groups[1].Value) -ForegroundColor Red
    $Ok = $false
}
if ($Found -eq 0 -or $Passed.Count + $Failed.Count -ne $Found) {
    Write-Host "  Not every test reported a result (crash or timeout?)." -ForegroundColor Red
    $Ok = $false
}
if ($ExpectedCount -gt 0 -and $Found -ne $ExpectedCount) {
    Write-Host "  Expected $ExpectedCount tests, found $Found (a renamed or hidden test?)." -ForegroundColor Red
    $Ok = $false
}
if (-not $ExitLine -or $ExitLine.Matches[0].Groups[1].Value -ne '0') {
    Write-Host "  The automation run did not end with exit code 0." -ForegroundColor Red
    $Ok = $false
}

$After = Get-Snapshot $Watched
$Changed = @()
foreach ($Key in $Before.Keys) {
    if (-not $After.ContainsKey($Key)) { $Changed += "removed  $Key" }
    elseif ($After[$Key] -ne $Before[$Key]) { $Changed += "changed  $Key" }
}
foreach ($Key in $After.Keys) {
    if (-not $Before.ContainsKey($Key)) { $Changed += "added    $Key" }
}
if ($Changed.Count -gt 0) {
    Write-Host "  The tests changed the user's data:" -ForegroundColor Red
    $Changed | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
    $Ok = $false
} else {
    Write-Host "User data unchanged ($($Before.Count) files checked)."
}

Write-Host "Log: $LogFile"
if ($Ok) { Write-Host 'PASS' -ForegroundColor Green; exit 0 }
Write-Host 'FAIL' -ForegroundColor Red
exit 1
