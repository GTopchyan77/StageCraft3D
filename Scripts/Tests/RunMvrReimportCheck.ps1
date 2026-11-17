# MVR re-import (STATE.md #99, Docs/ADR/0034 section 18, MV5) through the real request path: a scripted -game run that imports a
# self-authored plot, then a changed version of it saved under ANOTHER file name (designers number their versions; the plot is
# recognised by its MVR layer), undoes and redoes the update, and exports the stage. It checks the log (review counts, outcome,
# every fixture's ID, place, name and origin) and the exported file (the kept MVR data written back under the same UUID), and
# takes a screenshot of the update review.
#
# Fixtures carry no GDTF file, so they become the Sketch window's generic fixture and nothing is added to the fixture library.
# RunGameScripted backs up and restores profile, scenes, workspace and settings.
#
#   powershell -File Scripts\Tests\RunMvrReimportCheck.ps1 [-Name MvrA]
# Prints the game's lines that matter and PASS / FAIL.
param(
	[string]$Name = 'MvrA'
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

# RunGameScripted empties the run folder first, so the plot files live next to it. No commas or spaces: steps split on commas.
$Files = Join-Path $env:TEMP "StageCraftRuns\$Name-files"
if (Test-Path $Files) { Remove-Item -Recurse -Force $Files }
New-Item -ItemType Directory -Force $Files | Out-Null
$V1 = Join-Path $Files 'Plot_v1.mvr'
$V2 = Join-Path $Files 'Plot_v2.mvr'
$Out = Join-Path $Files 'Exported.mvr'

$Layer = '9C000000-0000-4000-8000-000000000001'
$IdA = '9C000000-0000-4000-8000-00000000000A'
$IdB = '9C000000-0000-4000-8000-00000000000B'
$IdC = '9C000000-0000-4000-8000-00000000000C'
$IdD = '9C000000-0000-4000-8000-00000000000D'

# Version 1: A (901, 3.001, carries CastShadow and a vendor element), B (902), C (903). Version 2: A 1 m further along X, B renamed,
# C gone, D new asking for 903.
function Fixture([string]$Id, [string]$Label, [int]$X, [int]$FixtureId, [string]$Address, [string]$Extra = '') {
	$Patch = if ($Address) { "<Addresses><Address break=`"0`">$Address</Address></Addresses>" } else { '' }
	"  <Fixture uuid=`"$Id`" name=`"$Label`"><Matrix>{1,0,0}{0,1,0}{0,0,1}{$X,-2000,6000}</Matrix><FixtureIDNumeric>$FixtureId</FixtureIDNumeric>$Patch$Extra</Fixture>"
}
function Scene([string[]]$Fixtures) {
	@('<?xml version="1.0" encoding="UTF-8"?>',
		'<GeneralSceneDescription verMajor="1" verMinor="6" provider="StageCraft checks" providerVersion="1">',
		"<Scene><Layers><Layer uuid=`"$Layer`" name=`"Check Rig`"><ChildList>") + $Fixtures + @('</ChildList></Layer></Layers></Scene></GeneralSceneDescription>') -join "`n"
}
function Write-Mvr([string]$Path, [string]$Xml) {
	$Stream = [System.IO.File]::Create($Path)
	$Zip = New-Object System.IO.Compression.ZipArchive($Stream, [System.IO.Compression.ZipArchiveMode]::Create)
	$Entry = $Zip.CreateEntry('GeneralSceneDescription.xml', [System.IO.Compression.CompressionLevel]::Optimal)
	$Writer = New-Object System.IO.StreamWriter($Entry.Open(), (New-Object System.Text.UTF8Encoding($false)))
	$Writer.Write($Xml)
	$Writer.Dispose()
	$Zip.Dispose()
	$Stream.Dispose()
}
$Kept = '<CastShadow>true</CastShadow><VendorNote by="checks">keep me</VendorNote>'
Write-Mvr $V1 (Scene @((Fixture $IdA 'Check A' 0 901 '3.1' $Kept), (Fixture $IdB 'Check B' 1000 902 '3.20'), (Fixture $IdC 'Check C' 2000 903 '3.40')))
Write-Mvr $V2 (Scene @((Fixture $IdA 'Check A' 1000 901 '3.1' $Kept), (Fixture $IdB 'Check B wash' 1000 902 '3.20'), (Fixture $IdD 'Check D' 3000 903 '3.60')))

& (Join-Path $PSScriptRoot 'RunGameScripted.ps1') -Name $Name -WithoutUserImports -Extra '-StageNoWelcome' -Steps `
	"2 StageCraft.Mvr.Import $V1", `
	'5 StageCraft.Mvr.Status', `
	'6 StageCraft.Mvr.Confirm', `
	'9 StageCraft.Mvr.Status', `
	"10 StageCraft.Mvr.Import $V2", `
	'13 StageCraft.Mvr.Status', `
	'13 StageCraft.Workspace.ScreenshotWindow', `
	'15 StageCraft.Mvr.Confirm', `
	'18 StageCraft.Mvr.Status', `
	'19 StageCraft.Edit.Undo', `
	'21 StageCraft.Mvr.Status', `
	'22 StageCraft.Edit.Redo', `
	'24 StageCraft.Mvr.Status', `
	"25 StageCraft.Mvr.Export $Out", `
	'28 StageCraft.Edit.Undo', `
	'29 StageCraft.Edit.Undo', `
	'31 quit'

$Run = Join-Path $env:TEMP "StageCraftRuns\$Name"
$Log = Join-Path $Run 'game.log'
$Lines = if (Test-Path $Log) { @(Get-Content $Log) } else { @() }
$Checks = [System.Collections.Generic.List[object]]::new()
function Check([string]$What, [bool]$Ok, [string]$Detail = '') { $Checks.Add([pscustomobject]@{ Ok = $Ok; What = $What; Detail = $Detail }) }

# Each scheduled Status prints a block: its fixture lines, up to the "N imported items" line.
function StatusBlocks {
	$Blocks = @()
	for ($i = 0; $i -lt $Lines.Count; $i++) {
		if ($Lines[$i] -notmatch "Delay: running 'StageCraft\.Mvr\.Status'") { continue }
		$Block = @()
		for ($j = $i + 1; $j -lt [Math]::Min($i + 40, $Lines.Count); $j++) {
			$Block += $Lines[$j]
			if ($Lines[$j] -match 'imported items on the stage') { break }
		}
		$Blocks += , $Block
	}
	return , $Blocks
}
function FixtureLine($Block, [string]$Id) { @($Block | Where-Object { $_ -match "mvr $Id" }) | Select-Object -First 1 }
$Blocks = StatusBlocks
Check 'six status blocks' ($Blocks.Count -eq 6) "$($Blocks.Count)"
if ($Blocks.Count -eq 6) {
	$Placed = $Blocks[1]; $Review = $Blocks[2]; $Updated = $Blocks[3]; $Undone = $Blocks[4]; $Redone = $Blocks[5]
	$A1 = FixtureLine $Placed $IdA
	Check 'version 1: A, B, C placed with their IDs, from Plot_v1.mvr, A keeps its data' ((FixtureLine $Placed $IdC) -match 'fixture 903 "Check C"' -and $A1 -match 'fixture 901 "Check A".* at \(-?0, 200, 600\).* from Plot_v1\.mvr kept [1-9]' -and (FixtureLine $Placed $IdB) -match 'fixture 902 "Check B"') $A1
	$ReviewLine = @($Review | Where-Object { $_ -match 'review new' }) | Select-Object -First 1
	Check 'version 2 review: 1 new, 2 changed, 0 unchanged, 1 missing, 1 keeping data' ($ReviewLine -match 'review new 1, changed 2, unchanged 0, missing 1, preserved 1') $ReviewLine
	$A2 = FixtureLine $Updated $IdA
	Check 'update: A moved 1 m (still 901), B renamed, C gone, D takes 903' ($A2 -match 'fixture 901 "Check A".* at \(100, 200, 600\).* from Plot_v2\.mvr kept [1-9]' -and (FixtureLine $Updated $IdB) -match 'fixture 902 "Check B wash"' -and -not (FixtureLine $Updated $IdC) -and (FixtureLine $Updated $IdD) -match 'fixture 903 "Check D"') $A2
	Check 'undo: A back, B named Check B, C back as 903, no D' ((FixtureLine $Undone $IdA) -match 'at \(-?0, 200, 600\)' -and (FixtureLine $Undone $IdB) -match '"Check B" ' -and (FixtureLine $Undone $IdC) -match 'fixture 903 "Check C"' -and -not (FixtureLine $Undone $IdD))
	Check 'redo: the update again' ((FixtureLine $Redone $IdA) -match 'at \(100, 200, 600\)' -and (FixtureLine $Redone $IdD) -match 'fixture 903' -and -not (FixtureLine $Redone $IdC))
}
Check 'the outcome names the update' (($Lines -match 'MVR import: Updated from Plot_v2\.mvr: 1 added, 2 updated, 1 removed, 0 unchanged\.').Count -ge 1)
Check 'one undo step named after the file' (($Lines -match 'History: undo "Update from Plot_v2\.mvr"').Count -ge 1)

# The export writes A's kept data back inside A's element.
$Exported = ''
if (Test-Path $Out) {
	$Zip = [System.IO.Compression.ZipFile]::OpenRead($Out)
	try {
		$Entry = $Zip.GetEntry('GeneralSceneDescription.xml')
		if ($Entry) { $Reader = New-Object System.IO.StreamReader($Entry.Open()); $Exported = $Reader.ReadToEnd(); $Reader.Dispose() }
	}
	finally { $Zip.Dispose() }
}
$AElement = [regex]::Match($Exported, "<Fixture name=`"Check A`" uuid=`"$IdA`">[\s\S]*?</Fixture>").Value
Check 'export: A under its UUID with CastShadow and the vendor element' ($AElement -match '<CastShadow>true</CastShadow>' -and $AElement -match '<VendorNote by="checks">keep me</VendorNote>') ($AElement -replace '\s+', ' ')
$Shot = Get-ChildItem $Run -File -Filter *.png -ErrorAction SilentlyContinue | Select-Object -First 1
Check 'a screenshot of the update review' ($null -ne $Shot) $(if ($Shot) { $Shot.FullName } else { '' })

$Lines | Where-Object { $_ -match 'StageCraft\.Mvr\.(Status|Confirm|Import|Export)|MVR import: (plan|Updated|Imported)|  fixture 90[0-9]|History: (undo|redo)' } |
	ForEach-Object { 'game: ' + ($_ -replace '^\[[^\]]*\]\[[^\]]*\]LogStageCraft: (Display: )?', '') }
foreach ($C in $Checks) { '{0}  {1}{2}' -f ($(if ($C.Ok) { 'ok  ' } else { 'FAIL' }), $C.What, $(if ($C.Detail) { " ($($C.Detail))" } else { '' })) }
if (@($Checks | Where-Object { -not $_.Ok }).Count -eq 0) { 'PASS'; exit 0 } else { 'FAIL'; exit 1 }
