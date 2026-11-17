# Toolbar dropdowns (STATE.md #100): a scripted -game run that opens the Sketch to Stage window and the Audio panel, reads every
# toolbar dropdown as it would open (StageCraft.Workspace.Menu Describe), picks entries through the same description a click runs
# (Pick), checks that the labels follow the change, puts the settings back, and takes screenshots with the Sketch window's Snap
# menu, the Audio panel's Source menu and the viewport's View menu open.
#
# Only the test beat is started (no microphone or system audio is listened to). RunGameScripted backs up and restores profile,
# scenes, workspace and settings.
#
#   powershell -File Scripts\Tests\RunToolbarMenuCheck.ps1 [-Name MenuA]
# Prints the game's menu lines and PASS / FAIL.
param(
	[string]$Name = 'MenuA'
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root

$M = 'StageCraft.Workspace.Menu'
& (Join-Path $PSScriptRoot 'RunGameScripted.ps1') -Name $Name -WithoutUserImports -Extra '-StageNoWelcome' -Steps `
	'3 StageCraft.Workspace.OpenPanel StageCraft.Panel.Audio', `
	'3 StageCraft.Workspace.OpenPanel StageCraft.Panel.Sketch', `
	"6 $M SketchEditMenu Describe", `
	"6 $M SketchSnapMenu Describe", `
	"6 $M SketchItemsMenu Describe", `
	"6 $M SketchPhotoMenu Describe", `
	"6 $M SourceMenu Describe", `
	"6 $M ViewMenuButton Describe", `
	"6 $M SnapMenuButton Describe", `
	"6 $M TransformMenuButton Describe", `
	"6 $M ToolsMenuButton Describe", `
	"7 $M SketchSnapMenu Pick 1 m", `
	"7 $M SketchSnapMenu Pick High", `
	"7 $M SketchEditMenu Pick Undo Stroke", `
	"7 $M SketchPhotoMenu Pick Claude", `
	"8 $M SourceMenu Pick Test Beat", `
	"10 $M SketchSnapMenu Describe", `
	"10 $M SourceMenu Describe", `
	"11 $M SketchSnapMenu Open", `
	'12 StageCraft.Workspace.ScreenshotWindow All', `
	"14 $M SourceMenu Open", `
	'15 StageCraft.Workspace.ScreenshotWindow All', `
	"17 $M ViewMenuButton Open", `
	'18 StageCraft.Workspace.ScreenshotWindow All', `
	"20 $M SourceMenu Pick Off", `
	"20 $M SketchSnapMenu Pick 50 cm", `
	"20 $M SketchSnapMenu Pick Low", `
	"21 $M SourceMenu Describe", `
	"21 $M SketchSnapMenu Describe", `
	'23 quit'

$Run = Join-Path $env:TEMP "StageCraftRuns\$Name"
$Log = Join-Path $Run 'game.log'
$Lines = if (Test-Path $Log) { @(Get-Content $Log -Encoding UTF8) } else { @() }
$Checks = [System.Collections.Generic.List[object]]::new()
function Check([string]$What, [bool]$Ok, [string]$Detail = '') { $Checks.Add([pscustomobject]@{ Ok = $Ok; What = $What; Detail = $Detail }) }

# The menu lines, in order, without the log prefix.
$Menu = @($Lines | Where-Object { $_ -match 'LogStageWorkspace: Display: Menu ' } | ForEach-Object { $_ -replace '^.*LogStageWorkspace: Display: Menu ', '' })
function Has([string]$Pattern) { @($Menu | Where-Object { $_ -match $Pattern }).Count -ge 1 }
# The lines of one Describe of Dropdown, the Nth (0-based) time it was described.
function Described([string]$Dropdown, [int]$Nth) {
	$Blocks = @(); $Current = $null
	foreach ($Line in $Menu) {
		if ($Line -match "^$Dropdown`: label ") { if ($Current) { $Blocks += , $Current }; $Current = @($Line); continue }
		if ($Current -and $Line -match "^$Dropdown`: ") { $Current += $Line; continue }
		if ($Current) { $Blocks += , $Current; $Current = $null }
	}
	if ($Current) { $Blocks += , $Current }
	if ($Nth -lt $Blocks.Count) { return , $Blocks[$Nth] } else { return , @() }
}
function InBlock($Block, [string]$Pattern) { @($Block | Where-Object { $_ -match $Pattern }).Count -ge 1 }

$Edit = Described 'SketchEditMenu' 0
Check 'Sketch Edit: Undo Stroke and Clear Sketch greyed out on an empty sketch, Frame Sketch offered' ((InBlock $Edit 'Undo Stroke -$') -and (InBlock $Edit 'Clear Sketch -$') -and (InBlock $Edit 'Frame Sketch$')) ($Edit -join ' | ')
$Snap0 = Described 'SketchSnapMenu' 0
Check 'Sketch Snap: grid steps, Snap to Grid, smoothing, one step checked' ((InBlock $Snap0 '\[Grid Step\]') -and (InBlock $Snap0 '\[[x ]\] Snap to Grid') -and (InBlock $Snap0 '\[Freehand Smoothing\]') -and @($Snap0 | Where-Object { $_ -match '\(x\) (25 cm|50 cm|1 m)$' }).Count -eq 1) ($Snap0 -join ' | ')
$Items = Described 'SketchItemsMenu' 0
Check 'Sketch Items: a submenu per element naming its item' ((InBlock $Items '> Deck: ') -and (InBlock $Items '> Truss: ') -and (InBlock $Items '> Fixture: ')) ($Items -join ' | ')
$Photo = Described 'SketchPhotoMenu' 0
Check 'Sketch Photo: import offered, remove greyed out without a photo, This PC reads' ((InBlock $Photo 'Import Photo\.\.\.$') -and (InBlock $Photo 'Remove Photo -$') -and (InBlock $Photo '\(x\) This PC')) ($Photo -join ' | ')
$Source0 = Described 'SourceMenu' 0
Check 'Audio Source: Off checked, five sources and background playback' ((InBlock $Source0 'label "Off"') -and (InBlock $Source0 '\(x\) Off$') -and (InBlock $Source0 '\( \) Test Beat$') -and (InBlock $Source0 '\[[x ]\] Keep Playing in the Background$')) ($Source0 -join ' | ')
Check 'viewport menus still describe (View, Snap, Transform, Tools)' ((Has '^ViewMenuButton: +\(x\) ') -and (Has '^SnapMenuButton: \[Grid Snap\]') -and (Has '^TransformMenuButton: +\(x\) ') -and (Has '^ToolsMenuButton: +Sketch to Stage$'))

Check 'picks ran: 1 m, High, Test Beat' ((Has '^SketchSnapMenu: pick "1 m" ran') -and (Has '^SketchSnapMenu: pick "High" ran') -and (Has '^SourceMenu: pick "Test Beat" ran'))
Check 'greyed-out picks refused: Undo Stroke on an empty sketch' (Has '^SketchEditMenu: pick "Undo Stroke" REFUSED')
$Snap1 = Described 'SketchSnapMenu' 1
Check 'the Snap label follows the picks' ((InBlock $Snap1 'label "Snap 1 m . Smooth High"') -and (InBlock $Snap1 '\(x\) 1 m$') -and (InBlock $Snap1 '\(x\) High$')) ($Snap1 -join ' | ')
$Source1 = Described 'SourceMenu' 1
Check 'the Source label follows the test beat' ((InBlock $Source1 'label "Test Beat"') -and (InBlock $Source1 '\(x\) Test Beat$')) ($Source1 -join ' | ')
$Source2 = Described 'SourceMenu' 2
$Snap2 = Described 'SketchSnapMenu' 2
Check 'settings put back: source Off, 50 cm, Low' ((InBlock $Source2 'label "Off"') -and (InBlock $Snap2 'label "Snap 50 cm . Smooth Low"')) (($Source2 + $Snap2) -join ' | ')
Check 'three menus opened' (@($Menu | Where-Object { $_ -match ': opened\.' }).Count -eq 3)
$Shots = @(Get-ChildItem $Run -File -Filter *.png -ErrorAction SilentlyContinue)
Check 'screenshots taken' ($Shots.Count -ge 3) "$($Shots.Count) in $Run"

$Menu | ForEach-Object { 'game: ' + $_ }
foreach ($C in $Checks) { '{0}  {1}{2}' -f ($(if ($C.Ok) { 'ok  ' } else { 'FAIL' }), $C.What, $(if ($C.Detail) { " ($($C.Detail))" } else { '' })) }
if (@($Checks | Where-Object { -not $_.Ok }).Count -eq 0) { 'PASS'; exit 0 } else { 'FAIL'; exit 1 }
