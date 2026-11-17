# SHOW ME for a dropdown entry (STATE.md #101): a scripted -game run that asks the assistant's SHOW ME for the Audio panel's
# "Off" source while the panel is closed (the path a user takes), checks that the panel opens and the SOURCE dropdown opens with it,
# that a plain control (FLASH) is only pulsed, that the knowledge search names the whole path (Audio > SOURCE > Off), and that every
# anchor still resolves to a live widget. Takes a screenshot with the dropdown open.
#
# Nothing is sent to Claude (StageCraft.Assistant.Search is local) and nothing is listened to (no source is picked). RunGameScripted
# backs up and restores profile, scenes, workspace and settings.
#
#   powershell -File Scripts\Tests\RunShowMeDropdownCheck.ps1 [-Name ShowMeA]
# Prints the game's SHOW ME and search lines and PASS / FAIL.
param(
	[string]$Name = 'ShowMeA'
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root

& (Join-Path $PSScriptRoot 'RunGameScripted.ps1') -Name $Name -WithoutUserImports -Extra '-StageNoWelcome' -Steps `
	'4 StageCraft.Assistant.Search how do I turn the music off', `
	'5 StageCraft.Assistant.ShowMe StageCraft.Panel.Audio SourceOff', `
	'7 StageCraft.Workspace.ScreenshotWindow All', `
	'9 StageCraft.Assistant.ShowMe StageCraft.Panel.Audio FlashLimit', `
	'10 StageCraft.Assistant.ShowMe StageCraft.Panel.Playback Blackout', `
	'12 StageCraft.Assistant.CheckAnchors', `
	'15 quit'

$Run = Join-Path $env:TEMP "StageCraftRuns\$Name"
$Log = Join-Path $Run 'game.log'
$Lines = if (Test-Path $Log) { @(Get-Content $Log -Encoding UTF8) } else { @() }
$Checks = [System.Collections.Generic.List[object]]::new()
function Check([string]$What, [bool]$Ok, [string]$Detail = '') { $Checks.Add([pscustomobject]@{ Ok = $Ok; What = $What; Detail = $Detail }) }
function Has([string]$Pattern) { @($Lines | Where-Object { $_ -match $Pattern }).Count -ge 1 }

$Shown = @($Lines | Where-Object { $_ -match 'SHOW ME: |Knowledge search|^\S*LogStageCraft: Display:   \d|Anchor check: \d' } | ForEach-Object { $_ -replace '^\[[^\]]*\]\[[^\]]*\]', '' })

Check 'search names the dropdown path (Audio > SOURCE > Off)' (Has 'control\.Audio\.SourceOff .*Audio . SOURCE . Off$')
Check 'SHOW ME on a closed panel opens it and opens the SOURCE dropdown' (Has 'SHOW ME: StageCraft\.Panel\.Audio SourceOff \(its dropdown opened\)\.')
Check 'a control on the panel is only pulsed (FLASH)' (Has 'SHOW ME: StageCraft\.Panel\.Audio FlashLimit\.$')
Check 'another panel still works (Playback BLACKOUT)' (Has 'SHOW ME: StageCraft\.Panel\.Playback Blackout\.$')
Check 'every anchor resolves to a live widget' (Has 'Anchor check: \d+ anchors resolve to live widgets, 0 do not\.')
Check 'no SHOW ME gave up' (-not (Has 'SHOW ME: .* does not show'))
$Shots = @(Get-ChildItem $Run -File -Filter *.png -ErrorAction SilentlyContinue)
Check 'screenshots taken, the open dropdown among them' (@($Shots | Where-Object { $_.Name -like 'Floating*' }).Count -ge 1) "$($Shots.Count) in $Run"

$Shown | ForEach-Object { 'game: ' + $_ }
foreach ($C in $Checks) { '{0}  {1}{2}' -f ($(if ($C.Ok) { 'ok  ' } else { 'FAIL' }), $C.What, $(if ($C.Detail) { " ($($C.Detail))" } else { '' })) }
if (@($Checks | Where-Object { -not $_.Ok }).Count -eq 0) { 'PASS'; exit 0 } else { 'FAIL'; exit 1 }
