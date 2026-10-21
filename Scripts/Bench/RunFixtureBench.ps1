<#
.SYNOPSIS
    Runs the fixture stress benchmark (Docs/ADR/0012-production-reports-and-fixture-benchmark.md §3) in a Standalone game window
    and prints its summary.

.DESCRIPTION
    Starts the project with -game, waits for the stage to load, runs StageCraft.Bench.Fixtures with the given counts, waits
    for the "StageCraft.Bench: done" line in the log, closes the game and prints the summary lines and the CSV path.

    Close other Unreal Editor instances first: they share the GPU and distort the GPU numbers.
    Development builds only (the benchmark is compiled out of Shipping). Build the editor target before running.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File Scripts\Bench\RunFixtureBench.ps1
    powershell -ExecutionPolicy Bypass -File Scripts\Bench\RunFixtureBench.ps1 -Counts 50,200,500 -Seconds 15 -Fade 2
    powershell -ExecutionPolicy Bypass -File Scripts\Bench\RunFixtureBench.ps1 -Zoom 0
#>
param(
    [int[]]$Counts = @(50, 200, 500),
    [int]$Seconds = 10,
    [double]$Fade = 2,
    # 1 = the looks differ in zoom too (worst case: every cone change rebuilds the light); 0 = zoom fixed.
    [ValidateSet(0, 1)][int]$Zoom = 1,
    [int]$ResX = 1600,
    [int]$ResY = 900,
    [int]$StartDelaySeconds = 10,
    [int]$TimeoutSeconds = 900
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$Project = Get-ChildItem -Path $ProjectRoot -Filter *.uproject | Select-Object -First 1
if (-not $Project) { throw "No .uproject in $ProjectRoot" }

$Association = (Get-Content $Project.FullName -Raw | ConvertFrom-Json).EngineAssociation
$EngineRoot = (Get-ItemProperty -Path "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$Association" -Name InstalledDirectory -ErrorAction SilentlyContinue).InstalledDirectory
if (-not $EngineRoot) { $EngineRoot = "C:\Program Files\Epic Games\UE_$Association" }
$Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.exe'
if (-not (Test-Path $Editor)) { throw "Unreal Editor not found at $Editor" }

if (Get-Process -Name 'UnrealEditor*' -ErrorAction SilentlyContinue) {
    Write-Warning 'Another Unreal Editor is running: GPU and frame numbers will include its load.'
}

$Log = Join-Path $ProjectRoot "Saved\Logs\$($Project.BaseName).log"
$BenchCommand = "StageCraft.Bench.Fixtures $($Counts -join ' ') Seconds=$Seconds Fade=$Fade Zoom=$Zoom"
$ExecCmds = "StageCraft.Workspace.Delay $StartDelaySeconds $BenchCommand"

Write-Host "Running: $BenchCommand"
$Game = Start-Process -FilePath $Editor -PassThru -ArgumentList @(
    "`"$($Project.FullName)`"", '-game', '-windowed', "-ResX=$ResX", "-ResY=$ResY", '-log', "-ExecCmds=`"$ExecCmds`""
)

$Deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$Done = $false
while ((Get-Date) -lt $Deadline -and -not $Game.HasExited) {
    Start-Sleep -Seconds 5
    if ((Test-Path $Log) -and (Select-String -Path $Log -Pattern 'StageCraft\.Bench: done' -Quiet)) { $Done = $true; break }
}

if (-not $Game.HasExited) {
    Stop-Process -Id $Game.Id -Force
    # Let it exit fully, so a following run does not measure (or warn about) this one.
    $Game.WaitForExit(30000) | Out-Null
}
if (-not $Done) { throw "The benchmark did not finish within $TimeoutSeconds s; see $Log" }

Select-String -Path $Log -Pattern 'StageCraft\.Bench:' | ForEach-Object { $_.Line -replace '^.*?StageCraft\.Bench:', 'StageCraft.Bench:' }
