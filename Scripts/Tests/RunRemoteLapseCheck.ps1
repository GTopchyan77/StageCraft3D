# Remote servers on a lapsed licence (STATE.md #98, Docs/ADR/0035 section 13, 0036 section 13).
# Runs StageCraft (-game, scripted) with the Web Remote and OSC on this PC under the development Pro override, lets the override lapse
# and come back, and meanwhile watches from outside the game whether the Web Remote's TCP port listens and the OSC UDP port is bound.
# MIDI stays off, so no device of this PC is touched. The user's Web Remote devices file is put back as it was; RunGameScripted
# restores profile, scenes and settings.
#
#   powershell -File Scripts\Tests\RunRemoteLapseCheck.ps1 [-Name LapseA] [-WebPort 48793] [-OscPort 48794]
# Prints the port timeline, the game's lines that matter and PASS / FAIL.
param(
	[string]$Name = 'LapseA',
	[int]$WebPort = 48793,
	[int]$OscPort = 48794
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root

$Run = Join-Path $env:TEMP "StageCraftRuns\$Name"
$Log = Join-Path $Run 'game.log'
if (Test-Path $Log) { Remove-Item -Force $Log }

# Starting and stopping the Web Remote saves remembered devices; whatever happens, the user's own list is put back.
$Devices = Join-Path $env:LOCALAPPDATA 'StageCraft\WebRemoteDevices.json'
$DevicesBackup = Join-Path $env:TEMP "StageCraftRuns\$Name-WebRemoteDevices.json.bak"
$DevicesExisted = Test-Path $Devices
if ($DevicesExisted) { Copy-Item -Force $Devices $DevicesBackup }

# A port already taken (a game still running, an earlier check) would make the timeline meaningless.
if ((Get-NetTCPConnection -LocalPort $WebPort -State Listen -ErrorAction SilentlyContinue) -or (Get-NetUDPEndpoint -LocalPort $OscPort -ErrorAction SilentlyContinue)) {
	"FAIL (port $WebPort/TCP or $OscPort/UDP is already in use; close what holds it or pass -WebPort / -OscPort)"
	exit 1
}

# The ports as another machine would find them: sampled every 250 ms, one line per change.
$Probe = Start-Job -ArgumentList $WebPort, $OscPort -ScriptBlock {
	param($WebPort, $OscPort)
	$Last = ''
	$End = (Get-Date).AddMinutes(4)
	while ((Get-Date) -lt $End) {
		$Web = [bool](Get-NetTCPConnection -LocalPort $WebPort -State Listen -ErrorAction SilentlyContinue)
		$Osc = [bool](Get-NetUDPEndpoint -LocalPort $OscPort -ErrorAction SilentlyContinue)
		$State = "web=$Web osc=$Osc"
		if ($State -ne $Last) { $State; $Last = $State }
		Start-Sleep -Milliseconds 250
	}
}
try {
	# Pro (the development switch) -> both on -> lapse -> switching on is refused -> renewed -> both off.
	& (Join-Path $PSScriptRoot 'RunGameScripted.ps1') -Name $Name -WithoutUserImports -Extra '-StageNoWelcome' -Steps `
		'2 StageCraft.Licence.DevAssumePro 1', `
		"3 StageCraft.WebRemote.Settings Local Port=$WebPort", `
		"4 StageCraft.Remote.Settings Local Port=$OscPort Feedback=0", `
		'7 StageCraft.WebRemote.Status', `
		'7 StageCraft.Remote.Status', `
		'9 StageCraft.Licence.DevAssumePro 0', `
		'12 StageCraft.WebRemote.Status', `
		'12 StageCraft.Remote.Status', `
		"13 StageCraft.WebRemote.Settings Local Port=$WebPort", `
		'16 StageCraft.Licence.DevAssumePro 1', `
		'19 StageCraft.WebRemote.Status', `
		'19 StageCraft.Remote.Status', `
		'21 StageCraft.WebRemote.Settings Off', `
		'21 StageCraft.Remote.Settings Off', `
		'24 quit'
}
finally {
	$Timeline = @(Receive-Job $Probe)
	Stop-Job $Probe -ErrorAction SilentlyContinue
	Remove-Job $Probe -Force -ErrorAction SilentlyContinue
	if ($DevicesExisted) { Copy-Item -Force $DevicesBackup $Devices; Remove-Item -Force $DevicesBackup }
	elseif (Test-Path $Devices) { Remove-Item -Force $Devices }
}

$Checks = [System.Collections.Generic.List[object]]::new()
function Check([string]$What, [bool]$Ok, [string]$Detail = '') { $Checks.Add([pscustomobject]@{ Ok = $Ok; What = $What; Detail = $Detail }) }

# Seen from outside: closed, open under Pro, closed on the lapse, open again on renewal, closed when switched off.
$Timeline | ForEach-Object { "port: $_" }
$Expected = @('web=False osc=False', 'web=True osc=True', 'web=False osc=False', 'web=True osc=True', 'web=False osc=False')
$Collapsed = @($Timeline | Where-Object { $_ -in @('web=False osc=False', 'web=True osc=True') })
Check 'ports: closed, open, closed on the lapse, open on renewal, closed when off' (($Collapsed -join '|') -eq ($Expected -join '|')) ($Timeline -join ' > ')

$Text = if (Test-Path $Log) { Get-Content $Log -Raw } else { '' }
$Lines = if (Test-Path $Log) { @(Get-Content $Log) } else { @() }
# The two lines each scheduled Status command printed (the Settings commands print a status of their own, not counted here).
function StatusLines([string]$Command, [string]$Pattern) {
	$Found = @()
	for ($i = 0; $i -lt $Lines.Count; $i++) {
		if ($Lines[$i] -notmatch "Delay: running '$Command'") { continue }
		$Found += @($Lines[($i + 1)..([Math]::Min($i + 4, $Lines.Count - 1))] | Where-Object { $_ -match $Pattern } | Select-Object -First 1)
	}
	return , $Found
}
$Web = StatusLines 'StageCraft\.WebRemote\.Status' 'StageCraft\.WebRemote\.Status: (listening|off) on port'
$Osc = StatusLines 'StageCraft\.Remote\.Status' 'StageCraft\.Remote\.Status: OSC (on|off)'
$WebProblems = StatusLines 'StageCraft\.WebRemote\.Status' 'StageCraft\.WebRemote\.Status: received'
$OscProblems = StatusLines 'StageCraft\.Remote\.Status' 'StageCraft\.Remote\.Status: received'
Check 'Web Remote: listening, off on the lapse, listening on renewal' ($Web.Count -eq 3 -and $Web[0] -match 'listening' -and $Web[1] -match ': off on port 0' -and $Web[2] -match 'listening')
Check 'OSC: on, off on the lapse, on on renewal' ($Osc.Count -eq 3 -and $Osc[0] -match 'OSC on' -and $Osc[1] -match 'OSC off' -and $Osc[2] -match 'OSC on')
Check 'the lapse is shown as the reason' ($WebProblems.Count -eq 3 -and $WebProblems[1] -match 'problem "The Web Remote needs StageCraft Pro' -and $OscProblems.Count -eq 3 -and $OscProblems[1] -match 'problem "MIDI and OSC control needs StageCraft Pro')
Check 'no warning while allowed' ($WebProblems.Count -eq 3 -and $WebProblems[2] -match 'problem ""' -and $OscProblems[2] -match 'problem ""')
Check 'the game logged both closings' ($Text -match 'Web Remote: stopped \(' -and $Text -match 'Remote: MIDI and OSC closed \(')
Check 'switching on while lapsed is refused' ($Text -match 'StageCraft\.WebRemote\.Settings: refused The Web Remote needs StageCraft Pro')
Check 'devices file as before' $(if ($DevicesExisted) { Test-Path $Devices } else { -not (Test-Path $Devices) })

$Lines | Where-Object { $_ -match 'StageCraft\.(Web)?Remote\.(Status|Settings)|Web Remote: (stopped|listening|closed)|Remote: MIDI and OSC closed|Remote: OSC' } |
	ForEach-Object { 'game: ' + ($_ -replace '^\[[^\]]*\]\[[^\]]*\]LogStageCraft: (Display: )?', '') }
foreach ($C in $Checks) { '{0}  {1}{2}' -f ($(if ($C.Ok) { 'ok  ' } else { 'FAIL' }), $C.What, $(if ($C.Detail) { " ($($C.Detail))" } else { '' })) }
if (@($Checks | Where-Object { -not $_.Ok }).Count -eq 0) { 'PASS'; exit 0 } else { 'FAIL'; exit 1 }
