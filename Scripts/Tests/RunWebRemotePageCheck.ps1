# Web Remote page check in a real browser (WR4, Docs/ADR/0036 section 12).
# Runs StageCraft (-game, scripted) with the Web Remote on this PC, and meanwhile drives the phone page in headless Edge or Chrome with
# WebRemotePageBrowserCheck.mjs: pairing from the QR link, the Layout sheet, reload, GO, lock and unlock, reset, unpair.
# The user's Web Remote devices file is put back as it was; RunGameScripted restores profile, scenes and settings.
#
#   powershell -File Scripts\Tests\RunWebRemotePageCheck.ps1 [-Name WR4a] [-Port 48792]
# Prints the browser checks, the game's Web Remote status lines and PASS / FAIL.
param(
	[string]$Name = 'WR4a',
	[int]$Port = 48792
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root

$Run = Join-Path $env:TEMP "StageCraftRuns\$Name"
$PageOut = Join-Path $env:TEMP "StageCraftRuns\$Name-page"
New-Item -ItemType Directory -Force $PageOut | Out-Null
Get-ChildItem $PageOut -File -ErrorAction SilentlyContinue | Remove-Item -Force
$Log = Join-Path $Run 'game.log'
if (Test-Path $Log) { Remove-Item -Force $Log }

# Paired during the check and unpaired at its end; if the check stops halfway, the user's own list is restored below.
$Devices = Join-Path $env:LOCALAPPDATA 'StageCraft\WebRemoteDevices.json'
$DevicesBackup = Join-Path $PageOut 'WebRemoteDevices.json.bak'
$DevicesExisted = Test-Path $Devices
if ($DevicesExisted) { Copy-Item -Force $Devices $DevicesBackup }

$Driver = Join-Path $PSScriptRoot 'WebRemotePageBrowserCheck.mjs'
$Browser = Start-Job -ArgumentList $Driver, $Log, $Port, $PageOut -ScriptBlock {
	param($Driver, $Log, $Port, $PageOut)
	$env:NODE_NO_WARNINGS = '1'
	node $Driver $Log $Port $PageOut 2>&1
	"exit:$LASTEXITCODE"
}
try {
	# The Web Remote is Pro: the development switch stands in for a licence (absent from Shipping).
	& (Join-Path $PSScriptRoot 'RunGameScripted.ps1') -Name $Name -WithoutUserImports -Extra '-StageNoWelcome' -Steps `
		'2 StageCraft.Licence.DevAssumePro 1', `
		"3 StageCraft.WebRemote.Settings Local Port=$Port", `
		'4 StageCraft.WebRemote.Status Pin', `
		'60 StageCraft.WebRemote.Status', `
		'61 StageCraft.WebRemote.Settings Off', `
		'63 quit'
	$Output = @(Receive-Job $Browser -Wait -AutoRemoveJob)
}
finally {
	Get-Job | Remove-Job -Force -ErrorAction SilentlyContinue
	if ($DevicesExisted) { Copy-Item -Force $DevicesBackup $Devices; Remove-Item -Force $DevicesBackup }
	elseif (Test-Path $Devices) { Remove-Item -Force $Devices }
}

$Json = $Output | Where-Object { $_ -like '{*' } | Select-Object -Last 1
$Exit = ($Output | Where-Object { $_ -like 'exit:*' } | Select-Object -Last 1) -replace 'exit:', ''
if (-not $Json) { $Output | ForEach-Object { "browser: $_" }; 'FAIL (no result from the browser)'; exit 1 }
$Result = $Json | ConvertFrom-Json
foreach ($Check in $Result.checks) {
	$Detail = if ($null -ne $Check.detail) { ' ' + ($Check.detail | ConvertTo-Json -Compress -Depth 4) } else { '' }
	'{0}  {1}{2}' -f ($(if ($Check.ok) { 'ok  ' } else { 'FAIL' }), $Check.name, $Detail)
}
$Result.shots | ForEach-Object { "Shot: $_" }
if (Test-Path $Log) {
	# The status taken while the server still listened (the run switches it off afterwards): its state line and its counters.
	$Lines = @(Select-String -Path $Log -Pattern 'StageCraft\.WebRemote\.Status: (?!.*Pin=)' | ForEach-Object Line)
	$Last = -1
	for ($i = 0; $i -lt $Lines.Count; $i++) { if ($Lines[$i] -match 'Status: listening') { $Last = $i } }
	if ($Last -ge 0) { $Lines[$Last..([Math]::Min($Last + 1, $Lines.Count - 1))] | ForEach-Object { $_ -replace '^.*?StageCraft\.WebRemote\.Status', 'game: StageCraft.WebRemote.Status' } }
}
"devices file as before: $(if ($DevicesExisted) { Test-Path $Devices } else { -not (Test-Path $Devices) })"
if ($Result.ok -and $Exit -eq '0') { 'PASS'; exit 0 } else { 'FAIL'; exit 1 }
