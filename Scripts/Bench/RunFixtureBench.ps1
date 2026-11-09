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
    powershell -ExecutionPolicy Bypass -File Scripts\Bench\RunFixtureBench.ps1 -Effects 1 -Audio
    powershell -ExecutionPolicy Bypass -File Scripts\Bench\RunFixtureBench.ps1 -Item DA_Fixture_Spot470
    powershell -ExecutionPolicy Bypass -File Scripts\Bench\RunFixtureBench.ps1 -TestBarCells 64
#>
param(
    [int[]]$Counts = @(50, 200, 500),
    [int]$Seconds = 10,
    [double]$Fade = 2,
    # 1 = the looks differ in zoom too (worst case: every cone change rebuilds the light); 0 = zoom fixed.
    [ValidateSet(0, 1)][int]$Zoom = 1,
    # Fixture data asset to bench (e.g. DA_MovingHead_Test, DA_Fixture_Spot470); empty takes the catalog's first fixture.
    [string]$Item = '',
    # Grand Master during the run: below 1 every fade frame goes through the full mix (HTP, LTP, masters; ADR 0026 section 6).
    [double]$GM = 1,
    # Effects on every bench fixture: 0 none, 1 circle + dimmer sine, 2 also a rainbow, 3 held by the two cues (fade and morph on every Go; ADR 0027 sections 5 and 7).
    [ValidateSet(0, 1, 2, 3)]
    [int]$Effects = 0,
    # Also send every bench universe as Art-Net and sACN unicast to 127.0.0.1 on unused ports (ADR 0019 section 6.7).
    [switch]$DMXOut,
    # Make the effects follow the test signal at 128 BPM (needs -Effects 1..3; ADR 0029 section 8).
    [switch]$Audio,
    # Also stand an audience area of this many people under the fixtures (ADR 0031 section 6); 0 for none.
    [ValidateRange(0, 20000)][int]$Crowd = 0,
    # Also add the self-authored test pixel bar with this many cells for the session (StageCraft.Fixtures.AddTestBar; ADR 0033 G4) and bench it; 0 for none.
    [ValidateRange(0, 64)][int]$TestBarCells = 0,
    # Camera for the run as "X Y Z Pitch Yaw" (metres, degrees; StageCraft.View.Camera), so the crowd is in view; empty keeps the stage view.
    [string]$Camera = '',
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
$BenchCommand = "StageCraft.Bench.Fixtures $($Counts -join ' ') Seconds=$Seconds Fade=$Fade Zoom=$Zoom GM=$GM Effects=$Effects"
if ($TestBarCells -gt 0) { $Item = 'Pixels' }
if ($Item) { $BenchCommand += " Item=$Item" }
if ($DMXOut) { $BenchCommand += " DMXOut=1" }
if ($Audio) { $BenchCommand += " Audio=1" }
if ($Crowd -gt 0) { $BenchCommand += " Crowd=$Crowd" }
$ExecCmds = "StageCraft.Workspace.Delay $StartDelaySeconds $BenchCommand"
if ($TestBarCells -gt 0) { $ExecCmds = "StageCraft.Workspace.Delay $($StartDelaySeconds - 2) StageCraft.Fixtures.AddTestBar $TestBarCells," + $ExecCmds }
if ($Camera) { $ExecCmds = "StageCraft.Workspace.Delay $($StartDelaySeconds - 1) StageCraft.View.Camera $Camera," + $ExecCmds }

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
