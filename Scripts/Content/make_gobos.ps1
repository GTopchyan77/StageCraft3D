# Generates the starter gobo wheel for StageCraft fixtures as 512x512 greyscale PNGs (white = light passes), into
# Scripts/Content/Gobos/. create_lighting_content.py imports them into /Game/StageCraft/Lighting/Gobos.
#
# The full texture spans the beam cone (Docs/ADR/0007 §5.2), so every pattern stays inside the inscribed circle and the
# border is black: a clamped texture then never leaks light outside the beam.
#
# Deterministic: the "breakup" patterns use a fixed seed, so re-running produces identical files.
#
#   powershell -ExecutionPolicy Bypass -File Scripts/Content/make_gobos.ps1

Add-Type -AssemblyName System.Drawing

$Size = 512
$Centre = $Size / 2.0
$Radius = $Size * 0.46
$OutDir = Join-Path $PSScriptRoot "Gobos"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function New-Canvas {
    $bitmap = New-Object System.Drawing.Bitmap($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.Clear([System.Drawing.Color]::Black)
    return @($bitmap, $graphics)
}

function Save-Canvas($canvas, $name) {
    $path = Join-Path $OutDir ("T_Gobo_" + $name + ".png")
    $canvas[1].Dispose()
    $canvas[0].Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $canvas[0].Dispose()
    Write-Output "Wrote $path"
}

# Everything is drawn inside the inscribed circle: the clip keeps patterns off the border.
function Set-CircleClip($graphics) {
    $clip = New-Object System.Drawing.Drawing2D.GraphicsPath
    $clip.AddEllipse([float]($Centre - $Radius), [float]($Centre - $Radius), [float](2 * $Radius), [float](2 * $Radius))
    $graphics.SetClip($clip)
}

$White = [System.Drawing.Brushes]::White
$Black = [System.Drawing.Brushes]::Black

# 1. Dots: a ring of eight dots around a centre dot.
$c = New-Canvas; $g = $c[1]; Set-CircleClip $g
$dot = $Radius * 0.22
$g.FillEllipse($White, [float]($Centre - $dot), [float]($Centre - $dot), [float](2 * $dot), [float](2 * $dot))
for ($i = 0; $i -lt 8; $i++) {
    $a = $i * [Math]::PI / 4
    $x = $Centre + [Math]::Cos($a) * $Radius * 0.62; $y = $Centre + [Math]::Sin($a) * $Radius * 0.62
    $g.FillEllipse($White, [float]($x - $dot), [float]($y - $dot), [float](2 * $dot), [float](2 * $dot))
}
Save-Canvas $c "Dots"

# 2. Star: a five-pointed star.
$c = New-Canvas; $g = $c[1]; Set-CircleClip $g
$points = New-Object 'System.Drawing.PointF[]' 10
for ($i = 0; $i -lt 10; $i++) {
    $r = if ($i % 2 -eq 0) { $Radius * 0.95 } else { $Radius * 0.40 }
    $a = -[Math]::PI / 2 + $i * [Math]::PI / 5
    $points[$i] = New-Object System.Drawing.PointF([float]($Centre + [Math]::Cos($a) * $r), [float]($Centre + [Math]::Sin($a) * $r))
}
$g.FillPolygon($White, $points)
Save-Canvas $c "Star"

# 3. Bars: parallel slots.
$c = New-Canvas; $g = $c[1]; Set-CircleClip $g
$bar = $Size / 13.0
for ($i = 0; $i -lt 13; $i += 2) { $g.FillRectangle($White, [float](($i) * $bar), [float]0, [float]$bar, [float]$Size) }
Save-Canvas $c "Bars"

# 4. Ring: a thick annulus.
$c = New-Canvas; $g = $c[1]; Set-CircleClip $g
$g.FillEllipse($White, [float]($Centre - $Radius), [float]($Centre - $Radius), [float](2 * $Radius), [float](2 * $Radius))
$inner = $Radius * 0.62
$g.FillEllipse($Black, [float]($Centre - $inner), [float]($Centre - $inner), [float](2 * $inner), [float](2 * $inner))
Save-Canvas $c "Ring"

# 5. Fan: six wedges, like a radial "flower" gobo.
$c = New-Canvas; $g = $c[1]; Set-CircleClip $g
for ($i = 0; $i -lt 6; $i++) {
    $g.FillPie($White, [float]($Centre - $Radius), [float]($Centre - $Radius), [float](2 * $Radius), [float](2 * $Radius), [float]($i * 60), [float]30)
}
Save-Canvas $c "Fan"

# 6. Breakup: organic "leaf" breakup of overlapping blobs (fixed seed).
$c = New-Canvas; $g = $c[1]; Set-CircleClip $g
$rng = New-Object System.Random(7301)
for ($i = 0; $i -lt 70; $i++) {
    $r = $Radius * (0.05 + 0.10 * $rng.NextDouble())
    $a = $rng.NextDouble() * 2 * [Math]::PI; $d = $Radius * [Math]::Sqrt($rng.NextDouble())
    $x = $Centre + [Math]::Cos($a) * $d; $y = $Centre + [Math]::Sin($a) * $d
    $g.FillEllipse($White, [float]($x - $r), [float]($y - $r * 0.7), [float](2 * $r), [float](1.4 * $r))
}
Save-Canvas $c "Breakup"

# 7. Triangles: a ring of six triangles pointing out.
$c = New-Canvas; $g = $c[1]; Set-CircleClip $g
for ($i = 0; $i -lt 6; $i++) {
    $a = $i * [Math]::PI / 3
    $tip = New-Object System.Drawing.PointF([float]($Centre + [Math]::Cos($a) * $Radius * 0.95), [float]($Centre + [Math]::Sin($a) * $Radius * 0.95))
    $l = New-Object System.Drawing.PointF([float]($Centre + [Math]::Cos($a - 0.28) * $Radius * 0.35), [float]($Centre + [Math]::Sin($a - 0.28) * $Radius * 0.35))
    $rgt = New-Object System.Drawing.PointF([float]($Centre + [Math]::Cos($a + 0.28) * $Radius * 0.35), [float]($Centre + [Math]::Sin($a + 0.28) * $Radius * 0.35))
    $g.FillPolygon($White, [System.Drawing.PointF[]]@($tip, $l, $rgt))
}
Save-Canvas $c "Triangles"

# 8. Grid: a square window grid.
$c = New-Canvas; $g = $c[1]; Set-CircleClip $g
$cell = $Size / 8.0; $gap = $cell * 0.18
for ($ix = 0; $ix -lt 8; $ix++) { for ($iy = 0; $iy -lt 8; $iy++) {
    $g.FillRectangle($White, [float]($ix * $cell + $gap), [float]($iy * $cell + $gap), [float]($cell - 2 * $gap), [float]($cell - 2 * $gap))
} }
Save-Canvas $c "Grid"
