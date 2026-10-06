# Synthesises the editor feedback sounds (StageCraft.Sound.*) as 16-bit mono WAV files.
# Reproducible source for /Game/StageCraft/Audio/Feedback/SFX_*: re-run this, then
# Scripts/Content/create_placement_audio_content.py, to regenerate the assets.
#
# Usage: powershell -ExecutionPolicy Bypass -File Scripts/Content/make_feedback_wavs.ps1 [-OutDir <folder>]

param(
    [string]$OutDir = (Join-Path $PSScriptRoot "..\..\Saved\ContentSource\Feedback")
)

$ErrorActionPreference = "Stop"
$SampleRate = 44100

function Write-Wav([string]$Path, [double[]]$Samples) {
    $data = New-Object byte[] ($Samples.Length * 2)
    for ($i = 0; $i -lt $Samples.Length; $i++) {
        $v = [Math]::Max(-1.0, [Math]::Min(1.0, $Samples[$i]))
        $s = [int16][Math]::Round($v * 32767)
        $bytes = [BitConverter]::GetBytes($s)
        $data[2 * $i] = $bytes[0]
        $data[2 * $i + 1] = $bytes[1]
    }

    $stream = [System.IO.File]::Create($Path)
    $w = New-Object System.IO.BinaryWriter($stream)
    $w.Write([Text.Encoding]::ASCII.GetBytes("RIFF")); $w.Write([int32](36 + $data.Length))
    $w.Write([Text.Encoding]::ASCII.GetBytes("WAVE"))
    $w.Write([Text.Encoding]::ASCII.GetBytes("fmt ")); $w.Write([int32]16); $w.Write([int16]1); $w.Write([int16]1)
    $w.Write([int32]$SampleRate); $w.Write([int32]($SampleRate * 2)); $w.Write([int16]2); $w.Write([int16]16)
    $w.Write([Text.Encoding]::ASCII.GetBytes("data")); $w.Write([int32]$data.Length); $w.Write($data)
    $w.Close()
}

# A tone whose frequency glides from F0 to F1, with a 3 ms attack and an exponential decay.
function New-Tone([double]$Seconds, [double]$F0, [double]$F1, [double]$Decay, [double]$Gain, [double]$Harmonic3 = 0.0) {
    $n = [int]($Seconds * $SampleRate)
    $out = New-Object double[] $n
    $phase = 0.0
    for ($i = 0; $i -lt $n; $i++) {
        $t = $i / $SampleRate
        $f = $F0 + ($F1 - $F0) * ($i / $n)
        $phase += 2 * [Math]::PI * $f / $SampleRate
        $attack = [Math]::Min(1.0, $t / 0.003)
        $env = $attack * [Math]::Exp(-$t * $Decay)
        $out[$i] = $Gain * $env * ([Math]::Sin($phase) + $Harmonic3 * [Math]::Sin(3 * $phase))
    }
    # 2 ms fade-out so no file ends on a click.
    $fade = [Math]::Min($n, [int](0.002 * $SampleRate))
    for ($i = 0; $i -lt $fade; $i++) { $out[$n - 1 - $i] *= $i / $fade }
    return ,$out
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Select: short bright upward blip.
Write-Wav (Join-Path $OutDir "SFX_Select.wav") (New-Tone 0.08 1200 1600 45 0.6)

# Place: soft low "thunk" gliding down.
Write-Wav (Join-Path $OutDir "SFX_Place.wav") (New-Tone 0.16 220 95 22 0.9 0.15)

# Snap: tiny high tick.
Write-Wav (Join-Path $OutDir "SFX_Snap.wav") (New-Tone 0.02 2600 2400 180 0.5)

# Error: two buzzy descending beeps with a short gap.
$beep1 = New-Tone 0.09 240 230 12 0.55 0.35
$gap = New-Object double[] ([int](0.05 * $SampleRate))
$beep2 = New-Tone 0.11 200 185 12 0.55 0.35
Write-Wav (Join-Path $OutDir "SFX_Error.wav") ([double[]]($beep1 + $gap + $beep2))

Get-ChildItem $OutDir -Filter *.wav | ForEach-Object { "{0}  {1} bytes" -f $_.FullName, $_.Length }
