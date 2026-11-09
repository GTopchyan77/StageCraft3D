# End-to-end run of the UE client against a real StageCraft Cloud server on this computer (Docs/ADR/0032, STATE.md #77).
#
# 1. Starts Services/StageCraftCloud in development mode (SQLite, dev sign-in, fake payment provider, local blob store) in a fresh
#    temporary folder.
# 2. Over HTTP, as a seller: signs in, onboards, publishes a real-money package ($15.00); as an admin: approves it.
# 3. Starts StageCraft (-game, RunGameScripted.ps1) pointed at the server. The game signs in and buys the listing with one
#    StageCraft.Marketplace.Get; this script plays the browser from the game's log (StageCraft.Cloud.LogBrowserUrls): it approves the
#    device code as "Buyer" and presses Pay on the fake checkout, which sends the server a signed webhook as Stripe would.
# 4. Checks the server's books (order paid, entitlement, 15 % / 85 % split) and the game's log (signed in, paid, downloaded, owned).
# No real money, no real Stripe, no browser. The game signs out at the end; the user's profile, scenes and marketplace folder are
# restored by RunGameScripted. The editor must be closed. Requires Node.js 22.
#   powershell -ExecutionPolicy Bypass -File Scripts\Tests\RunCloudEndToEnd.ps1
param(
	[int]$Port = 8787,
	[int]$PriceMinor = 1500
)
$ErrorActionPreference = 'Stop'
$Root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$Service = Join-Path $Root 'Services\StageCraftCloud'
$Work = Join-Path $env:TEMP 'StageCraftCloudE2E'
$Base = "http://127.0.0.1:$Port"
$RunName = 'CloudE2E'
$RunsDir = if ($env:STAGECRAFT_RUN_DIR) { $env:STAGECRAFT_RUN_DIR } else { Join-Path $env:TEMP 'StageCraftRuns' }
$GameLog = Join-Path (Join-Path $RunsDir $RunName) 'game.log'
$VaultFile = Join-Path $env:LOCALAPPDATA 'StageCraft\Credentials\StageCraftCloud.key'
$VaultExisted = Test-Path $VaultFile
$Failures = New-Object System.Collections.Generic.List[string]
function Check($Condition, $What) { if ($Condition) { Write-Output "  ok   $What" } else { Write-Output "  FAIL $What"; $Failures.Add($What) } }

if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory -Force $Work | Out-Null

# --- Server ---
$env:NODE_NO_WARNINGS = '1'; $env:NODE_ENV = 'development'; $env:PORT = "$Port"; $env:PUBLIC_BASE_URL = $Base; $env:AUTH_MODE = 'dev'; $env:PAYMENT_PROVIDER = 'fake'
$env:BLOB_STORE = 'local'; $env:BLOB_LOCAL_DIR = (Join-Path $Work 'blobs'); $env:SQLITE_PATH = (Join-Path $Work 'cloud.sqlite'); $env:DATABASE_URL = ''
$Server = Start-Process -FilePath 'node' -ArgumentList 'src/main.ts' -WorkingDirectory $Service -PassThru -WindowStyle Hidden `
	-RedirectStandardOutput (Join-Path $Work 'server.out') -RedirectStandardError (Join-Path $Work 'server.err')
try {
	$Ready = $false
	for ($i = 0; $i -lt 60 -and -not $Ready; $i++) {
		try { $Ready = (Invoke-WebRequest -UseBasicParsing "$Base/readyz" -TimeoutSec 2).StatusCode -eq 200 } catch { Start-Sleep -Milliseconds 500 }
	}
	if (-not $Ready) { throw "The server did not start; see $Work\server.err" }
	Write-Output "Server ready at $Base."

	function Api($Method, $Path, $Body = $null, $Token = '', $ContentType = 'application/json') {
		$Headers = @{}
		if ($Token) { $Headers['Authorization'] = "Bearer $Token" }
		$Arguments = @{ Method = $Method; Uri = "$Base$Path"; Headers = $Headers; UseBasicParsing = $true }
		if ($null -ne $Body) { $Arguments['Body'] = $Body; $Arguments['ContentType'] = $ContentType }
		Invoke-RestMethod @Arguments
	}
	function DevSignIn($Name) {
		$Device = Api 'POST' '/v1/auth/device'
		Invoke-WebRequest -UseBasicParsing "$Base/auth/verify/start?user_code=$($Device.userCode)&dev_user=$Name" | Out-Null
		(Api 'POST' '/v1/auth/device/token' (@{ deviceCode = $Device.deviceCode } | ConvertTo-Json)).sessionToken
	}

	# --- Seller publishes, admin approves ---
	$Seller = DevSignIn 'Seller'
	$Onboarding = Api 'POST' '/v1/me/seller/onboarding' (@{ country = 'US' } | ConvertTo-Json) $Seller
	if ($Onboarding.onboardingUrl) { Invoke-WebRequest -UseBasicParsing $Onboarding.onboardingUrl | Out-Null }
	Check ((Api 'GET' '/v1/me' $null $Seller).seller.payoutsEnabled) 'seller onboarded'
	$PackagePath = Join-Path $Work 'paid.stage'
	Push-Location $Service
	$ErrorActionPreference = 'Continue'
	$ListingId = (& node scripts/make-dev-package.ts $PackagePath $PriceMinor | Select-Object -Last 1).Trim()
	$ErrorActionPreference = 'Stop'
	Pop-Location
	$Published = Api 'POST' '/v1/listings' ([System.IO.File]::ReadAllBytes($PackagePath)) $Seller 'application/octet-stream'
	Check ($Published.status -eq 'PendingReview') 'published listing waits for review'
	$Admin = DevSignIn 'Admin'
	$AdminId = (Api 'GET' '/v1/me' $null $Admin).accountId
	Push-Location $Service
	$ErrorActionPreference = 'Continue'
	& node src/cli.ts grant-admin $AdminId | Out-Null
	$ErrorActionPreference = 'Stop'
	Pop-Location
	Api 'POST' "/v1/admin/moderation/$($Published.listingId)" (@{ decision = 'approve' } | ConvertTo-Json) $Admin | Out-Null
	Check ((Api 'GET' '/v1/listings').totalMatches -eq 1) 'approved listing is listed'

	# --- The browser, played from the game's log ---
	$Browser = Start-Job -ArgumentList $GameLog, $Base -ScriptBlock {
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
					if ($Url -match 'user_code=([^&]+)') {
						Invoke-WebRequest -UseBasicParsing "$Base/auth/verify/start?user_code=$($Matches[1])&dev_user=Buyer" | Out-Null
						"approved sign-in $($Matches[1])"
					} elseif ($Url -match '/dev/fake-checkout/') {
						Invoke-WebRequest -UseBasicParsing -Method POST "$Url/pay" | Out-Null
						"paid $Url"
					}
				}
			}
			Start-Sleep -Milliseconds 500
		}
	}

	# --- The game ---
	$Steps = @('2 StageCraft.Cloud.LogBrowserUrls 1', '3 StageCraft.Cloud.SignIn', '16 StageCraft.Cloud.Status', '17 StageCraft.Marketplace.Query',
		'20 StageCraft.Marketplace.Status', '21 StageCraft.Marketplace.Get 1', '50 StageCraft.Marketplace.Status', '51 StageCraft.Cloud.Status',
		'52 StageCraft.Cloud.SignOut', '55 quit')
	& (Join-Path $PSScriptRoot 'RunGameScripted.ps1') -Name $RunName -Steps $Steps -TimeoutSeconds 180 -WithoutUserImports `
		-Extra "-ini:Game:[/Script/ModularSceneBuilder.StageCloudSubsystem]:BaseUrl=$Base -StageNoWelcome"
	$BrowserLog = Receive-Job $Browser -Wait -AutoRemoveJob
	$BrowserLog | ForEach-Object { Write-Output "  browser: $_" }

	# --- Checks ---
	$Log = if (Test-Path $GameLog) { Get-Content $GameLog -Raw } else { '' }
	Check ($Log -match 'StageCraft\.Cloud\.Status: SignedIn') 'game signed in through the device flow'
	Check ($Log -match 'Cloud store: transaction \S+ paid') 'game saw the order paid (server webhook)'
	Check ($Log -match 'StageCraft\.Cloud\.SignOut: SignedOut') 'game signed out at the end'
	Check (-not ($Log -match 'Bearer scs_|scs_[A-Za-z0-9_-]{20,}')) 'no session token in the game log'
	$Buyer = DevSignIn 'Buyer'
	$Orders = (Api 'GET' '/v1/me/orders' $null $Buyer).orders
	Check (@($Orders | Where-Object { $_.state -eq 'Paid' -and $_.amountMinor -eq $PriceMinor }).Count -eq 1) 'one paid order on the server'
	Check (@((Api 'GET' '/v1/me/entitlements' $null $Buyer).entitlements).Count -eq 1) 'buyer owns the listing on the server'
	$Summary = Api 'GET' '/v1/admin/summary' $null $Admin
	$Usd = $Summary.currencies | Where-Object { $_.currency -eq 'USD' }
	Check ($Usd -and [int64]$Usd.grossSales -eq $PriceMinor) "gross sales $PriceMinor"
	Check ($Usd -and [int64]$Usd.sellerEarnings -eq [math]::Floor($PriceMinor * 0.85)) 'seller earns 85 %'
	Check ($Usd -and [int64]$Usd.platformRevenue -eq ($PriceMinor - [math]::Floor($PriceMinor * 0.85))) 'platform keeps 15 %'
	Check ($Log -match 'Marketplace acquire \S+: EStageMarketplaceResult::Success') 'game downloaded and imported the stage'
}
finally {
	if ($Server -and -not $Server.HasExited) { Stop-Process -Id $Server.Id -Force }
	Get-Job | Remove-Job -Force -ErrorAction SilentlyContinue
	if (-not $VaultExisted -and (Test-Path $VaultFile)) { Remove-Item -Force $VaultFile }
}
if ($Failures.Count -eq 0) { Write-Output 'PASS'; exit 0 } else { Write-Output "FAIL ($($Failures.Count))"; exit 1 }
