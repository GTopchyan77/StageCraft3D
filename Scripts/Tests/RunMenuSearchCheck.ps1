# Typing searches the toolbar dropdowns (STATE.md #102): a scripted -game run that clicks the Sketch window's Items dropdown with a
# synthetic left click (the real click path, SComboButton::OnButtonClicked), types "LED" as a keyboard would, checks that the menu
# took the characters (its search box has focus), takes a screenshot of the filtered list (which comes from inside the Fixture
# submenu), presses Escape twice (clear the search, close the menu) and checks that keyboard focus went back to where it was. Then the
# same through StageCraft.Workspace.Menu Open on the Audio SOURCE dropdown (the path SHOW ME uses).
#
# Nothing is picked and nothing is listened to. RunGameScripted backs up and restores profile, scenes, workspace and settings.
#
#   powershell -File Scripts\Tests\RunMenuSearchCheck.ps1 [-Name SearchA]
# Prints the game's lines and PASS / FAIL.
param(
	[string]$Name = 'SearchA'
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root

$M = 'StageCraft.Workspace.Menu'
$T = 'StageCraft.Workspace.Type'
& (Join-Path $PSScriptRoot 'RunGameScripted.ps1') -Name $Name -WithoutUserImports -Extra '-StageNoWelcome' -Steps `
	'3 StageCraft.Workspace.OpenPanel StageCraft.Panel.Audio', `
	'3 StageCraft.Workspace.OpenPanel StageCraft.Panel.Sketch', `
	"6 $T {Focus}", `
	"7 $M SketchItemsMenu Click", `
	"8 $T LED", `
	'9 StageCraft.Workspace.ScreenshotWindow All', `
	"10 $T {Escape}", `
	"11 $T {Escape}", `
	"12 $T {Focus}", `
	"14 $M SourceMenu Open", `
	"15 $T beat", `
	'16 StageCraft.Workspace.ScreenshotWindow All', `
	"17 $T {Escape}", `
	"18 $T {Escape}", `
	"19 $T {Focus}", `
	"20 $M SourceMenu Describe", `
	'22 quit'

$Run = Join-Path $env:TEMP "StageCraftRuns\$Name"
$Log = Join-Path $Run 'game.log'
$Lines = if (Test-Path $Log) { @(Get-Content $Log -Encoding UTF8) } else { @() }
$Checks = [System.Collections.Generic.List[object]]::new()
function Check([string]$What, [bool]$Ok, [string]$Detail = '') { $Checks.Add([pscustomobject]@{ Ok = $Ok; What = $What; Detail = $Detail }) }
$Shown = @($Lines | Where-Object { $_ -match 'LogStageWorkspace: Display: (Type |Menu )' } | ForEach-Object { $_ -replace '^.*LogStageWorkspace: Display: ', '' })
function Line([int]$Nth, [string]$Pattern) { $Found = @($Shown | Where-Object { $_ -match $Pattern }); if ($Nth -lt $Found.Count) { $Found[$Nth] } else { '' } }

$Focus = @($Shown | Where-Object { $_ -match '^Type \{Focus\}: focus ' } | ForEach-Object { $_ -replace '^Type \{Focus\}: focus (.*)\.$', '$1' })
$Click = Line 0 '^Menu SketchItemsMenu: clicked'
Check 'a click opens Items and focuses the menu itself' ($Click -match 'down handled, up handled\), focus SMultiBoxWidget\.$') $Click
$Led = Line 0 '^Type "LED"'
Check 'typing after a click searches Items (3 of 3 taken, search box focused)' ($Led -match '3 of 3 handled, focus SMultiBoxWidget -> SEditableText\.$') $Led
$Esc1 = Line 0 '^Type "\{Escape\}"'
$Esc2 = Line 1 '^Type "\{Escape\}"'
Check 'Escape clears the search, then closes the menu' (($Esc1 -match 'SEditableText -> SMultiBoxWidget') -and ($Esc2 -match 'SMultiBoxWidget -> nothing')) "$Esc1 | $Esc2"
Check 'focus goes back after the menu closes (Items)' ($Focus.Count -ge 2 -and $Focus[0] -ne 'nothing' -and $Focus[1] -eq $Focus[0]) ($Focus -join ' / ')
$Beat = Line 0 '^Type "beat"'
Check 'Open (the SHOW ME path) focuses the menu too: SOURCE search takes the characters' ($Beat -match '4 of 4 handled, focus SMultiBoxWidget -> SEditableText\.$') $Beat
Check 'focus goes back after the menu closes (SOURCE)' ($Focus.Count -ge 3 -and $Focus[2] -eq $Focus[0]) ($Focus -join ' / ')
Check 'nothing was picked: the source is still Off' ((Line 0 '^Menu SourceMenu: label ') -match 'label "Off"')
$Shots = @(Get-ChildItem $Run -File -Filter 'Floating*.png' -ErrorAction SilentlyContinue)
Check 'screenshots of both open menus' ($Shots.Count -ge 2) "$($Shots.Count) in $Run"

$Shown | ForEach-Object { 'game: ' + $_ }
foreach ($C in $Checks) { '{0}  {1}{2}' -f ($(if ($C.Ok) { 'ok  ' } else { 'FAIL' }), $C.What, $(if ($C.Detail) { " ($($C.Detail))" } else { '' })) }
if (@($Checks | Where-Object { -not $_.Ok }).Count -eq 0) { 'PASS'; exit 0 } else { 'FAIL'; exit 1 }
