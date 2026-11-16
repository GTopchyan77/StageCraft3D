# End-to-end run of StageCraft Pro (Docs/ADR/0034 section 17, STATE.md #94) against a real StageCraft Cloud server on this computer.
#
# 1. Starts Services/StageCraftCloud in development mode (SQLite, dev sign-in, fake payment provider, development signing key) in a
#    fresh temporary folder.
# 2. Run A (online): the game starts Free (StageCraft.Licence.DevAssumePro 0), DMX IN is refused and DMX OUT holds every universe but
#    the lowest (fixture 1 is moved to universe 2 first); it signs in, subscribes Monthly, and this script plays the browser from the
#    game's log (it approves the device code and presses Subscribe on the fake checkout, which sends the server signed
#    customer.subscription.created and checkout.session.completed webhooks as Stripe would). The game must switch to Pro by itself and lift both DMX limits.
# 3. Run B (offline): the server is stopped; the game must still start with Pro from the stored, signed licence. The stored session
#    stays SignedIn (Offline means no cloud configured), so the refresh fails to connect and must keep the licence.
# 4. Run C (online again): the session comes back from the vault, the script cancels in the fake billing portal, and REFRESH must
#    end Pro at once, stop DMX IN and refuse it again.
# 5. Checks the server's records (subscription canceled, one machine) and that no token or session token reached a log.
# No real money, no real Stripe, no browser. The user's licence file, cloud session, profile, scenes, settings and marketplace folder
# are restored. The editor must be closed. Requires Node.js 22.
#   powershell -ExecutionPolicy Bypass -File Scripts\Tests\RunLicenceEndToEnd.ps1
param(
	[int]$Port = 8788
)
$ErrorActionPreference = 'Stop'
$Root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$Service = Join-Path $Root 'Services\StageCraftCloud'
$Work = Join-Path $env:TEMP 'StageCraftLicenceE2E'
$Base = "http://127.0.0.1:$Port"
$RunsDir = if ($env:STAGECRAFT_RUN_DIR) { $env:STAGECRAFT_RUN_DIR } else { Join-Path $env:TEMP 'StageCraftRuns' }
$UserDir = Join-Path $env:LOCALAPPDATA 'StageCraft'
$LicenceFile = Join-Path $UserDir 'Licence.json'
$VaultFile = Join-Path $UserDir 'Credentials\StageCraftCloud.key'
$Failures = New-Object System.Collections.Generic.List[string]
function Check($Condition, $What) { if ($Condition) { Write-Output "  ok   $What" } else { Write-Output "  FAIL $What"; $Failures.Add($What) } }

if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory -Force $Work | Out-Null
$Backup = Join-Path $Work 'backup'
New-Item -ItemType Directory -Force $Backup | Out-Null
$LicenceExisted = Test-Path $LicenceFile
$VaultExisted = Test-Path $VaultFile
if ($LicenceExisted) { Copy-Item $LicenceFile (Join-Path $Backup 'Licence.json'); Remove-Item -Force $LicenceFile }
if ($VaultExisted) { Copy-Item $VaultFile (Join-Path $Backup 'StageCraftCloud.key'); Remove-Item -Force $VaultFile }

$env:NODE_NO_WARNINGS = '1'; $env:NODE_ENV = 'development'; $env:PORT = "$Port"; $env:PUBLIC_BASE_URL = $Base; $env:AUTH_MODE = 'dev'; $env:PAYMENT_PROVIDER = 'fake'
$env:BLOB_STORE = 'local'; $env:BLOB_LOCAL_DIR = (Join-Path $Work 'blobs'); $env:SQLITE_PATH = (Join-Path $Work 'cloud.sqlite'); $env:DATABASE_URL = ''
$script:Server = $null
function Start-Server($Tag) {
	$script:Server = Start-Process -FilePath 'node' -ArgumentList 'src/main.ts' -WorkingDirectory $Service -PassThru -WindowStyle Hidden `
		-RedirectStandardOutput (Join-Path $Work "server-$Tag.out") -RedirectStandardError (Join-Path $Work "server-$Tag.err")
	for ($i = 0; $i -lt 60; $i++) {
		try { if ((Invoke-WebRequest -UseBasicParsing "$Base/readyz" -TimeoutSec 2).StatusCode -eq 200) { Write-Output "Server ready at $Base."; return } } catch { Start-Sleep -Milliseconds 500 }
	}
	throw "The server did not start; see $Work\server-$Tag.err"
}
function Stop-Server { if ($script:Server -and -not $script:Server.HasExited) { Stop-Process -Id $script:Server.Id -Force; $script:Server.WaitForExit() } }

# Plays the browser for one game run: approves sign-ins as ProUser, pays the fake subscription checkout, cancels in the fake portal.
function Start-Browser($Log) {
	Start-Job -ArgumentList $Log, $Base -ScriptBlock {
		param($Log, $Base)
		$Done = @{}
		$Deadline = (Get-Date).AddMinutes(4)
		while ((Get-Date) -lt $Deadline) {
			if (Test-Path $Log) {
				foreach ($Match in (Select-String -Path $Log -Pattern 'Cloud: browser (\S+)' -AllMatches)) {
					$Url = $Match.Matches[0].Groups[1].Value
					if ($Done.ContainsKey($Url)) { continue }
					$Done[$Url] = $true
					Start-Sleep -Seconds 2
					try {
						if ($Url -match 'user_code=([^&]+)') {
							Invoke-WebRequest -UseBasicParsing "$Base/auth/verify/start?user_code=$($Matches[1])&dev_user=ProUser" | Out-Null
							"approved sign-in $($Matches[1])"
						} elseif ($Url -match '/dev/fake-subscription/') {
							Invoke-WebRequest -UseBasicParsing -Method POST "$Url/pay" | Out-Null
							'subscribed on the fake checkout'
						} elseif ($Url -match '/dev/fake-portal/') {
							Invoke-WebRequest -UseBasicParsing -Method POST "$Url/cancel" | Out-Null
							'cancelled in the fake portal'
						} else { "ignored $Url" }
					} catch { "browser error on ${Url}: $_" }
				}
			}
			Start-Sleep -Milliseconds 500
		}
	}
}

function Run-Game($Name, $Steps) {
	$GameLog = Join-Path (Join-Path $RunsDir $Name) 'game.log'
	if (Test-Path $GameLog) { Remove-Item -Force $GameLog }
	$Browser = Start-Browser $GameLog
	& (Join-Path $PSScriptRoot 'RunGameScripted.ps1') -Name $Name -Steps $Steps -TimeoutSeconds 180 -WithoutUserImports `
		-Extra "-ini:Game:[/Script/ModularSceneBuilder.StageCloudSubsystem]:BaseUrl=$Base -StageNoWelcome" |
		Where-Object { $_ -match '^(Restored|GAME TIMED OUT)' } | ForEach-Object {
			# Write-Host, not Check: this function's output is the game log its caller reads.
			if ($_ -match '^Restored: profile True, scenes True, marketplace True, imports True') { Write-Host "  ok   ${Name}: user data restored" }
			else { Write-Host "  FAIL ${Name}: $_"; $Failures.Add("${Name}: $_") }
		}
	Stop-Job $Browser -ErrorAction SilentlyContinue
	Receive-Job $Browser -AutoRemoveJob -Wait | ForEach-Object { Write-Output "  browser: $_" }
	if (Test-Path $GameLog) { Get-Content $GameLog } else { @() }
}
function Lines($Log, $Pattern) { @($Log | Where-Object { $_ -match $Pattern }) }

$Prefix = @('2 StageCraft.Cloud.LogBrowserUrls 1', '2 StageCraft.Licence.DevAssumePro 0')
try {
	Start-Server 'a'

	# --- Run A: Free, sign in, subscribe, Pro ---
	Write-Output 'Run A (online: subscribe)'
	$A = Run-Game 'LicenceE2E_A' ($Prefix + @('3 StageCraft.Scene.NewFromTemplate Concert', '8 StageCraft.Show.Select 1', '9 StageCraft.DMX.PatchSelection 2 1',
		'10 StageCraft.DMX.OutputSettings ArtNet=Unicast To=127.0.0.1 SACN=Off', '11 StageCraft.DMX.Output On', '12 StageCraft.Licence.Status',
		'13 StageCraft.DMX.Input On', '14 StageCraft.DMX.Output', '15 StageCraft.Cloud.SignIn', '27 StageCraft.Cloud.Status', '28 StageCraft.Licence.Status',
		'29 StageCraft.Licence.Subscribe Monthly', '46 StageCraft.Licence.Status', '47 StageCraft.DMX.Output', '48 StageCraft.DMX.Input On',
		'49 StageCraft.DMX.Input Off', '50 StageCraft.DMX.Output Off', '52 quit'))
	$Status = Lines $A 'StageCraft\.Licence\.Status: '
	$InputOn = Lines $A 'StageCraft\.DMX\.Input On: '
	$Output = Lines $A 'StageCraft\.DMX\.Output: EStageDMXOutputState::Sending'
	Check ($Status.Count -ge 3 -and $Status[0] -match 'Status: EStageLicenceStatus::Free;.*dev override off') 'A: starts Free with the development override off'
	Check ($InputOn.Count -ge 2 -and $InputOn[0] -match 'needs StageCraft Pro') 'A: DMX IN refused on Free'
	Check ($Output.Count -ge 2 -and $Output[0] -match 'sending universes \[1\].*held by the licence \[\d') 'A: Free DMX OUT sends universe 1 only and holds the rest'
	Check ($A -match 'StageCraft\.Cloud\.Status: SignedIn') 'A: signed in through the device flow'
	Check ($Status.Count -ge 3 -and $Status[1] -match 'Free;.*subscription none') 'A: signed in without a subscription stays Free'
	Check ($A -match 'Licence: refreshed \(monthly, until ') 'A: the game fetched a licence after the checkout webhook'
	Check ($Status.Count -ge 3 -and $Status[2] -match 'Status: EStageLicenceStatus::Pro; plan .monthly.;.*Pro\.DmxUniverses.*Pro\.DmxIn.*Pro\.MidiOsc.*Pro\.WebRemote.*Pro\.MvrExport.*subscription active \(entitled\)') 'A: Pro with all five entitlements'
	Check ($InputOn.Count -ge 2 -and $InputOn[1] -match 'done') 'A: DMX IN allowed on Pro'
	Check ($Output.Count -ge 2 -and $Output[-1] -match 'sending universes \[1, \d.*held by the licence \[\]') 'A: Pro DMX OUT sends every universe'
	Check (Test-Path $LicenceFile) 'A: licence stored on this computer'

	# --- Run B: offline ---
	Stop-Server
	Write-Output 'Run B (server stopped: offline licence)'
	$B = Run-Game 'LicenceE2E_B' ($Prefix + @('8 StageCraft.Licence.Status', '9 StageCraft.DMX.Input On', '10 StageCraft.DMX.Input Off', '11 StageCraft.Cloud.Status', '13 quit'))
	Check (@(Lines $B 'StageCraft\.Licence\.Status: EStageLicenceStatus::Pro;').Count -eq 1) 'B: Pro offline from the stored licence'
	Check (@(Lines $B 'StageCraft\.DMX\.Input On: done').Count -eq 1) 'B: DMX IN allowed offline'
	Check (@(Lines $B 'StageCraft\.Licence\.Status: EStageLicenceStatus::Pro;.*cannot be reached').Count -eq 1) 'B: the refresh could not reach the server and kept the licence'

	# --- Run C: cancel in the portal, refresh ---
	Start-Server 'c'
	Write-Output 'Run C (online: cancel)'
	$C = Run-Game 'LicenceE2E_C' ($Prefix + @('10 StageCraft.Cloud.Status', '11 StageCraft.Licence.Status', '12 StageCraft.DMX.Input On', '13 StageCraft.Licence.Manage',
		'22 StageCraft.Licence.Refresh', '28 StageCraft.Licence.Status', '29 StageCraft.DMX.Input', '30 StageCraft.DMX.Input On', '31 StageCraft.Cloud.SignOut', '33 quit'))
	$Status = Lines $C 'StageCraft\.Licence\.Status: '
	$InputOn = Lines $C 'StageCraft\.DMX\.Input On: '
	Check ($C -match 'StageCraft\.Cloud\.Status: SignedIn') 'C: session restored from the vault'
	Check ($Status.Count -ge 2 -and $Status[0] -match 'EStageLicenceStatus::Pro;') 'C: Pro while the subscription is active'
	Check ($InputOn.Count -ge 2 -and $InputOn[0] -match 'done') 'C: DMX IN on'
	Check ($Status.Count -ge 2 -and $Status[1] -match 'EStageLicenceStatus::Free;.*subscription canceled') 'C: cancelling ends Pro at the next refresh'
	Check (@(Lines $C 'StageCraft\.DMX\.Input: EStageDMXInputState::Off').Count -ge 1) 'C: DMX IN stopped when Pro ended'
	Check ($InputOn.Count -ge 2 -and $InputOn[1] -match 'needs StageCraft Pro') 'C: DMX IN refused again'
	Check (-not (Test-Path $LicenceFile)) 'C: licence removed from this computer'

	# --- Server records and logs ---
	$Db = Join-Path $Work 'cloud.sqlite'
	Push-Location $Service
	$ErrorActionPreference = 'Continue'
	$Rows = & node -e "const { DatabaseSync } = require('node:sqlite'); const db = new DatabaseSync(process.argv[1]); console.log(JSON.stringify({ subs: db.prepare('SELECT status, plan FROM subscriptions').all(), machines: db.prepare('SELECT COUNT(*) AS n FROM licence_machines').get().n }));" $Db
	$ErrorActionPreference = 'Stop'
	Pop-Location
	$Records = ($Rows | Select-Object -Last 1) | ConvertFrom-Json
	Check (@($Records.subs).Count -eq 1 -and $Records.subs[0].status -eq 'canceled' -and $Records.subs[0].plan -eq 'monthly') 'server: one monthly subscription, canceled'
	Check ($Records.machines -eq 1) 'server: one machine'
	$AllLogs = @($A) + @($B) + @($C)
	Check (-not ($AllLogs -match 'SC1\.[A-Za-z0-9_-]{20,}')) 'no licence token in the game logs'
	Check (-not ($AllLogs -match 'scs_[A-Za-z0-9_-]{20,}')) 'no session token in the game logs'
}
finally {
	Stop-Server
	Get-Job | Remove-Job -Force -ErrorAction SilentlyContinue
	if (Test-Path $LicenceFile) { Remove-Item -Force $LicenceFile }
	if ($LicenceExisted) { Copy-Item -Force (Join-Path $Backup 'Licence.json') $LicenceFile }
	if (Test-Path $VaultFile) { Remove-Item -Force $VaultFile }
	if ($VaultExisted) { Copy-Item -Force (Join-Path $Backup 'StageCraftCloud.key') $VaultFile }
	Write-Output "Restored: licence file $LicenceExisted, cloud session $VaultExisted."
}
if ($Failures.Count -eq 0) { Write-Output 'PASS'; exit 0 } else { Write-Output "FAIL ($($Failures.Count))"; exit 1 }
