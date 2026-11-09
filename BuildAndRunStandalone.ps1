# Builds, cooks and packages StageCraft for Win64 with RunUAT BuildCookRun into a local output folder, then starts the
# packaged executable, for manual testing in real binary conditions (network, DMX, file import; STATE.md #85).
#   powershell -ExecutionPolicy Bypass -File BuildAndRunStandalone.ps1 [-Config Development|Shipping] [-Clean] [-NoLaunch] [-SkipBuild]
#     -Config      Development (default; logs and console) or Shipping.
#     -Clean       Rebuilds the code and cooks every package again (BuildCookRun -clean) and empties the output folder.
#                  Without it the build and cook are incremental, which is the fast path.
#     -NoLaunch    Packages only.
#     -SkipBuild   Starts the last package of this configuration without building.
#     -OutputDir   Where packages go. Default: Saved\StandaloneBuild\<Config> (Saved is ignored by git).
#     -GameArgs    Extra command-line arguments for the game, for example '-ResX=1920 -ResY=1080'.
#     -EngineDir   Engine root. Default: the .uproject EngineAssociation from the registry, then UE_5.8 in Program Files.
# The packaged game keeps its own Saved folder next to the executable, so testing it never touches the editor's user data.
# It refuses to run while the editor has this project open or while a packaged instance is still running (locked files).
param(
	[ValidateSet('Development', 'Shipping')]
	[string]$Config = 'Development',
	[switch]$Clean,
	[switch]$NoLaunch,
	[switch]$SkipBuild,
	[string]$OutputDir = '',
	[string]$GameArgs = '',
	[string]$EngineDir = ''
)
$ErrorActionPreference = 'Stop'

$Project     = $PSScriptRoot
$ProjectName = 'ModularSceneBuilder'
$UProject    = Join-Path $Project "$ProjectName.uproject"
if (-not $OutputDir) { $OutputDir = Join-Path $Project "Saved\StandaloneBuild\$Config" }
$LogDir = Join-Path $Project 'Saved\StandaloneBuild\Logs'

function Resolve-EngineDir {
	if ($EngineDir) { return $EngineDir }
	if ($env:STAGECRAFT_ENGINE) { return $env:STAGECRAFT_ENGINE }
	$Association = (Get-Content -Raw $UProject | ConvertFrom-Json).EngineAssociation
	foreach ($Key in @("HKLM:\SOFTWARE\EpicGames\Unreal Engine\$Association", "HKLM:\SOFTWARE\WOW6432Node\EpicGames\Unreal Engine\$Association")) {
		$Entry = Get-ItemProperty -Path $Key -Name InstalledDirectory -ErrorAction SilentlyContinue
		if ($Entry -and (Test-Path $Entry.InstalledDirectory)) { return $Entry.InstalledDirectory }
	}
	$Builds = Get-ItemProperty -Path 'HKCU:\SOFTWARE\Epic Games\Unreal Engine\Builds' -ErrorAction SilentlyContinue
	if ($Builds -and $Builds.$Association -and (Test-Path $Builds.$Association)) { return $Builds.$Association }
	return "C:\Program Files\Epic Games\UE_$Association"
}

function Find-PackagedExe {
	if (-not (Test-Path $OutputDir)) { return $null }
	$Suffix = if ($Config -eq 'Shipping') { '-Win64-Shipping' } else { '' }
	# The executable at the package root is a launcher stub that starts this one; starting it directly gives the real PID.
	$Exe = Join-Path $OutputDir "Windows\$ProjectName\Binaries\Win64\$ProjectName$Suffix.exe"
	if (Test-Path $Exe) { Get-Item $Exe } else { $null }
}

function Assert-NothingLocked {
	$Running = Get-CimInstance Win32_Process -Filter "Name LIKE 'UnrealEditor%' OR Name LIKE '$ProjectName%'" -ErrorAction SilentlyContinue
	$Editors = @($Running | Where-Object { $_.Name -like 'UnrealEditor*' -and $_.CommandLine -like "*$ProjectName.uproject*" })
	if ($Editors.Count -gt 0) { throw "The editor has $ProjectName open (PID $($Editors[0].ProcessId)). Close it first: the cook and the build need its files." }
	$Packaged = @($Running | Where-Object { $_.Name -like "$ProjectName*" -and $_.ExecutablePath -and $_.ExecutablePath.StartsWith($OutputDir, [StringComparison]::OrdinalIgnoreCase) })
	if ($Packaged.Count -gt 0) { throw "A packaged $ProjectName is still running from $OutputDir (PID $($Packaged[0].ProcessId)). Close it first." }
}

$Engine = Resolve-EngineDir
$RunUAT = Join-Path $Engine 'Engine\Build\BatchFiles\RunUAT.bat'
if (-not (Test-Path $RunUAT)) { throw "RunUAT.bat not found under '$Engine'. Pass -EngineDir or set STAGECRAFT_ENGINE." }

if (-not $SkipBuild) {
	Assert-NothingLocked
	if ($Clean -and (Test-Path $OutputDir)) { Remove-Item -Recurse -Force $OutputDir }
	New-Item -ItemType Directory -Force $OutputDir, $LogDir | Out-Null
	$Log = Join-Path $LogDir "BuildCookRun-$Config.log"

	$UATArgs = @(
		'BuildCookRun', "-project=`"$UProject`"", '-noP4', '-utf8output', '-unattended',
		'-platform=Win64', "-clientconfig=$Config", '-build', '-cook', '-stage', '-pak', '-prereqs',
		'-archive', "-archivedirectory=`"$OutputDir`""
	)
	if ($Clean) { $UATArgs += '-clean' }

	Write-Output "Engine:  $Engine"
	Write-Output "Package: $Config -> $OutputDir$(if ($Clean) { ' (clean)' } else { ' (incremental)' })"
	Write-Output "Log:     $Log"
	$Started = Get-Date
	# PowerShell 5.1 turns a native tool's stderr lines into error records; with 'Stop' the first one would end the script.
	$ErrorActionPreference = 'Continue'
	& $RunUAT @UATArgs 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $Log | ForEach-Object {
		if ($_ -match 'BUILD (SUCCESSFUL|FAILED)|Error:|error C\d+|warning C\d+|Warning:|\*\*\*\* ') { $_ }
	}
	$ExitCode = $LASTEXITCODE
	$ErrorActionPreference = 'Stop'
	$Minutes = [math]::Round(((Get-Date) - $Started).TotalMinutes, 1)
	$Warnings = @(Select-String -Path $Log -Pattern '(^|\s)(Warning:|warning C\d+)' -ErrorAction SilentlyContinue).Count
	$Errors = @(Select-String -Path $Log -Pattern '(^|\s)(Error:|error C\d+)' -ErrorAction SilentlyContinue).Count
	if ($ExitCode -ne 0) { throw "BuildCookRun failed (exit $ExitCode, $Errors error lines, $Minutes min). See $Log" }
	Write-Output "BuildCookRun succeeded in $Minutes min: $Warnings warning lines, $Errors error lines."
}

$Exe = Find-PackagedExe
if (-not $Exe) { throw "No packaged $Config executable under $OutputDir. Run without -SkipBuild first." }
Write-Output "Executable: $($Exe.FullName)"
if ($NoLaunch) { return }

if ($SkipBuild) { Assert-NothingLocked }
$LaunchArgs = @('-windowed', '-ResX=1600', '-ResY=900')
if ($Config -ne 'Shipping') { $LaunchArgs += '-log' }
if ($GameArgs) { $LaunchArgs += $GameArgs.Split(' ', [StringSplitOptions]::RemoveEmptyEntries) }
$Game = Start-Process -FilePath $Exe.FullName -ArgumentList $LaunchArgs -WorkingDirectory $Exe.DirectoryName -PassThru
$SavedDir = if ($Config -eq 'Shipping') { Join-Path $env:LOCALAPPDATA "$ProjectName\Saved" } else { Join-Path $OutputDir "Windows\$ProjectName\Saved" }
Write-Output "Started $ProjectName ($Config), PID $($Game.Id). Its user data and logs are under $SavedDir."
