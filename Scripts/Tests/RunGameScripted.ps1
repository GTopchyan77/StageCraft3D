# Scripted Standalone -game run of StageCraft (STATE.md #58): runs console commands at set times (StageCraft.Workspace.Delay),
# collects the run's screenshots and log, and backs up and restores everything a run may touch: the profile, scenes, workspace
# layout, user settings and the marketplace folder (Rules.md Economy security 10). Run it from anywhere; the editor must be closed.
#   powershell -File Scripts\Tests\RunGameScripted.ps1 -Name R58 -Steps '3 StageCraft.Workspace.Screenshot','6 quit' [-Extra '-StageNoWelcome']
#   -CollectScenes A,B -CollectTo <folder>: scenes the run saved that are kept (copied out before the scenes folder is restored).
# Runs land in $env:STAGECRAFT_RUN_DIR (default %TEMP%\StageCraftRuns). Commas cannot appear inside a step (-ExecCmds splits on them).
param(
	[string]$Name = 'R58',
	[string[]]$Steps = @('3 StageCraft.Workspace.Screenshot', '6 quit'),
	[string]$Extra = '',
	[int]$TimeoutSeconds = 240,
	[string[]]$CollectScenes = @(),
	[string]$CollectTo = '',
	# Starts without the user's imported models (Saved/StageCraft/Imports.json is moved aside and put back), so heavy imports do not hold the run up.
	[switch]$WithoutUserImports
)
$ErrorActionPreference = 'Stop'
Set-Location (Resolve-Path (Join-Path $PSScriptRoot '..\..'))
$Project   = (Get-Location).Path
$Scratch   = if ($env:STAGECRAFT_RUN_DIR) { $env:STAGECRAFT_RUN_DIR } else { Join-Path $env:TEMP 'StageCraftRuns' }
$Run       = Join-Path $Scratch $Name
$Editor    = if ($env:STAGECRAFT_EDITOR) { $env:STAGECRAFT_EDITOR } else { 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' }
$Saved     = Join-Path $Project 'Saved'
$Scenes    = Join-Path $Saved 'StageCraft\Scenes'
$Market    = Join-Path $Saved 'StageCraft\Marketplace'
$Profile   = Join-Path $Saved 'SaveGames\StageCraftProfile.sav'
$Workspace = Join-Path $Saved 'StageCraft\Workspace'
$UserCfg   = Join-Path $Saved 'Config\WindowsEditor\GameUserSettings.ini'
$Shots     = Join-Path $Saved 'Screenshots'
$Imports   = Join-Path $Saved 'StageCraft\Imports.json'

if (Test-Path $Run) { Remove-Item -Recurse -Force $Run }
New-Item -ItemType Directory -Force $Run | Out-Null
$Backup = Join-Path $Run 'backup'
New-Item -ItemType Directory -Force $Backup | Out-Null

function Get-TreeHash($Path) { if (Test-Path $Path) { (Get-ChildItem $Path -Recurse -File | Sort-Object FullName | Get-FileHash | ForEach-Object Hash) -join '' } else { '' } }

$MarketExisted = Test-Path $Market
if ($MarketExisted) { Copy-Item -Recurse $Market (Join-Path $Backup 'Marketplace') }
$MarketHash = Get-TreeHash $Market
Copy-Item $Profile (Join-Path $Backup 'StageCraftProfile.sav')
Copy-Item -Recurse $Scenes (Join-Path $Backup 'Scenes')
$WorkspaceExisted = Test-Path $Workspace
if ($WorkspaceExisted) { Copy-Item -Recurse $Workspace (Join-Path $Backup 'Workspace') }
$UserCfgExisted = Test-Path $UserCfg
if ($UserCfgExisted) { Copy-Item $UserCfg (Join-Path $Backup 'GameUserSettings.ini') }
$ImportsExisted = Test-Path $Imports
if ($ImportsExisted) { Copy-Item $Imports (Join-Path $Backup 'Imports.json'); $ImportsHash = (Get-FileHash $Imports).Hash }
if ($WithoutUserImports -and $ImportsExisted) { Remove-Item -Force $Imports }
$ProfileHash = (Get-FileHash $Profile).Hash
$ScenesHash = Get-TreeHash $Scenes
$ShotsBefore = if (Test-Path $Shots) { @(Get-ChildItem $Shots -Recurse -File | ForEach-Object FullName) } else { @() }

$ExecCmds = ($Steps | ForEach-Object { "StageCraft.Workspace.Delay $_" }) -join ','
$Log = Join-Path $Run 'game.log'
$Args = @("`"$Project\ModularSceneBuilder.uproject`"", '-game', '-windowed', '-ResX=1600', '-ResY=900', '-nosplash', "-abslog=`"$Log`"", "-ExecCmds=`"$ExecCmds`"")
if ($Extra) { $Args += $Extra.Split(' ') }
# The game writes its log to stdout too: into files, never an inherited pipe (a caller that does not drain it would freeze the game).
$Game = Start-Process -FilePath $Editor -ArgumentList $Args -PassThru -RedirectStandardOutput (Join-Path $Run 'stdout.txt') -RedirectStandardError (Join-Path $Run 'stderr.txt')
if (-not $Game.WaitForExit($TimeoutSeconds * 1000)) { Stop-Process -Id $Game.Id -Force; Write-Output 'GAME TIMED OUT' }

# Screenshots of this run move into the run folder.
if (Test-Path $Shots) {
	Get-ChildItem $Shots -Recurse -File | Where-Object { $ShotsBefore -notcontains $_.FullName } | ForEach-Object { Move-Item $_.FullName (Join-Path $Run $_.Name) -Force }
}

# Scenes the run saved that the caller keeps (starter stages), copied out before the scenes folder is restored.
foreach ($SceneName in $CollectScenes) {
	$Source = Join-Path $Scenes ($SceneName + ".json")
	if (Test-Path $Source) { New-Item -ItemType Directory -Force $CollectTo | Out-Null; Copy-Item -Force $Source (Join-Path $CollectTo ($SceneName + ".json")); Write-Output "Collected: $SceneName" } else { Write-Output "NOT SAVED: $SceneName" }
}

# --- Restore ---
Remove-Item -Recurse -Force $Scenes
Copy-Item -Recurse (Join-Path $Backup 'Scenes') $Scenes
Copy-Item -Force (Join-Path $Backup 'StageCraftProfile.sav') $Profile
if (Test-Path $Workspace) { Remove-Item -Recurse -Force $Workspace }
if ($WorkspaceExisted) { Copy-Item -Recurse (Join-Path $Backup 'Workspace') $Workspace }
if ($UserCfgExisted) { Copy-Item -Force (Join-Path $Backup 'GameUserSettings.ini') $UserCfg } elseif (Test-Path $UserCfg) { Remove-Item -Force $UserCfg }
if ($ImportsExisted) { Copy-Item -Force (Join-Path $Backup 'Imports.json') $Imports } elseif (Test-Path $Imports) { Remove-Item -Force $Imports }
if (Test-Path $Market) { Remove-Item -Recurse -Force $Market }
if ($MarketExisted) { Copy-Item -Recurse (Join-Path $Backup 'Marketplace') $Market }
Write-Output "Restored: profile $((Get-FileHash $Profile).Hash -eq $ProfileHash), scenes $((Get-TreeHash $Scenes) -eq $ScenesHash), marketplace $((Get-TreeHash $Market) -eq $MarketHash), imports $(-not $ImportsExisted -or (Get-FileHash $Imports).Hash -eq $ImportsHash)"
Write-Output "Log: $Log"
Get-ChildItem $Run -Filter *.png | ForEach-Object { Write-Output "Shot: $($_.FullName)" }
