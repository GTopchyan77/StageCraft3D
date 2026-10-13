# Writes small 3D model files for testing runtime model import (Docs/ADR/0006-inspector-polish-and-runtime-model-import.md).
# Drag them onto the Library or the viewport, or use Library > IMPORT. Re-run to regenerate:
#   powershell -ExecutionPolicy Bypass -File Scripts/TestModels/make_test_models.ps1
#
#   Speaker_Cabinet.gltf   one box, glTF with an embedded buffer, dark material      0.60 x 0.50 x 1.10 m
#   DJ_Booth.glb           two meshes in two nodes (body + top plate), binary glTF   2.00 x 0.80 x 1.05 m
#   Stage_Riser.obj/.mtl   one box, Wavefront OBJ with a material (units: metres)    2.00 x 1.00 x 0.40 m
#   Not_A_Model.dae        unsupported extension, for the refusal path
#
# glTF is Y-up and in metres; every model stands on y = 0 with its pivot at the bottom centre.

$ErrorActionPreference = 'Stop'
$Out = $PSScriptRoot

# --- Box geometry: 24 vertices (flat normals), 36 indices, counter-clockwise outside ---

# Each face: normal n and in-plane axes u, v with u x v = n, so (-u-v, +u-v, +u+v, -u+v) is counter-clockwise from outside.
$Faces = @(
    @{ N = @( 1, 0, 0); U = @(0, 0, -1); V = @(0, 1, 0) },
    @{ N = @(-1, 0, 0); U = @(0, 0,  1); V = @(0, 1, 0) },
    @{ N = @( 0, 1, 0); U = @(1, 0,  0); V = @(0, 0, -1) },
    @{ N = @( 0,-1, 0); U = @(1, 0,  0); V = @(0, 0,  1) },
    @{ N = @( 0, 0, 1); U = @(1, 0,  0); V = @(0, 1,  0) },
    @{ N = @( 0, 0,-1); U = @(-1, 0, 0); V = @(0, 1,  0) }
)

function New-Box([double]$SizeX, [double]$SizeY, [double]$SizeZ, [double]$BaseY) {
    $Half = @(($SizeX / 2), ($SizeY / 2), ($SizeZ / 2))
    $Center = @(0, ($BaseY + $SizeY / 2), 0)
    $Positions = New-Object System.Collections.Generic.List[double]
    $Normals = New-Object System.Collections.Generic.List[double]
    $Indices = New-Object System.Collections.Generic.List[int]
    foreach ($Face in $Faces) {
        $First = $Positions.Count / 3
        foreach ($Corner in @(@(-1, -1), @(1, -1), @(1, 1), @(-1, 1))) {
            for ($Axis = 0; $Axis -lt 3; $Axis++) {
                $Positions.Add($Center[$Axis] + $Face.N[$Axis] * $Half[$Axis] + $Face.U[$Axis] * $Half[$Axis] * $Corner[0] + $Face.V[$Axis] * $Half[$Axis] * $Corner[1])
                $Normals.Add($Face.N[$Axis])
            }
        }
        foreach ($Offset in @(0, 1, 2, 0, 2, 3)) { $Indices.Add($First + $Offset) }
    }
    return @{ Positions = $Positions; Normals = $Normals; Indices = $Indices; Min = @(-$Half[0], $BaseY, -$Half[2]); Max = @($Half[0], ($BaseY + $SizeY), $Half[2]) }
}

# --- glTF buffers: positions, normals, indices per mesh, each view 4-byte aligned ---

function New-GltfDocument($Meshes, [string]$Generator) {
    $Stream = New-Object System.IO.MemoryStream
    $Writer = New-Object System.IO.BinaryWriter($Stream)
    $Views = @(); $Accessors = @(); $GltfMeshes = @(); $Nodes = @(); $Materials = @()

    for ($MeshIndex = 0; $MeshIndex -lt $Meshes.Count; $MeshIndex++) {
        $Mesh = $Meshes[$MeshIndex]
        $Box = $Mesh.Box

        $Start = $Stream.Position
        foreach ($Value in $Box.Positions) { $Writer.Write([single]$Value) }
        $Views += @{ buffer = 0; byteOffset = $Start; byteLength = $Stream.Position - $Start; target = 34962 }
        $Accessors += @{ bufferView = $Views.Count - 1; componentType = 5126; count = $Box.Positions.Count / 3; type = 'VEC3'; min = $Box.Min; max = $Box.Max }

        $Start = $Stream.Position
        foreach ($Value in $Box.Normals) { $Writer.Write([single]$Value) }
        $Views += @{ buffer = 0; byteOffset = $Start; byteLength = $Stream.Position - $Start; target = 34962 }
        $Accessors += @{ bufferView = $Views.Count - 1; componentType = 5126; count = $Box.Normals.Count / 3; type = 'VEC3' }

        $Start = $Stream.Position
        foreach ($Value in $Box.Indices) { $Writer.Write([uint16]$Value) }
        $Views += @{ buffer = 0; byteOffset = $Start; byteLength = $Stream.Position - $Start; target = 34963 }
        $Accessors += @{ bufferView = $Views.Count - 1; componentType = 5123; count = $Box.Indices.Count; type = 'SCALAR' }
        while ($Stream.Position % 4 -ne 0) { $Writer.Write([byte]0) }

        $First = $Accessors.Count - 3
        $Materials += @{ name = $Mesh.Material; pbrMetallicRoughness = @{ baseColorFactor = $Mesh.Color; metallicFactor = $Mesh.Metallic; roughnessFactor = 0.6 } }
        $GltfMeshes += @{ name = $Mesh.Name; primitives = @(@{ attributes = @{ POSITION = $First; NORMAL = $First + 1 }; indices = $First + 2; material = $MeshIndex }) }
        $Nodes += @{ name = $Mesh.Name; mesh = $MeshIndex; translation = $Mesh.Translation }
    }
    $Writer.Flush()

    $Document = [ordered]@{
        asset = @{ version = '2.0'; generator = $Generator }
        scene = 0
        scenes = @(@{ nodes = @(0..($Nodes.Count - 1)) })
        nodes = $Nodes
        meshes = $GltfMeshes
        materials = $Materials
        buffers = @(@{ byteLength = $Stream.Length })
        bufferViews = $Views
        accessors = $Accessors
    }
    return @{ Document = $Document; Bytes = $Stream.ToArray() }
}

$Generator = 'StageCraft make_test_models.ps1'

# Speaker_Cabinet.gltf: the buffer is embedded as a data URI, so the single file is complete.
$Cabinet = New-GltfDocument @(@{ Name = 'SpeakerCabinet'; Material = 'M_Cabinet'; Color = @(0.06, 0.06, 0.07, 1.0); Metallic = 0.0; Translation = @(0, 0, 0); Box = (New-Box 0.6 1.1 0.5 0.0) }) $Generator
$Cabinet.Document.buffers[0].uri = 'data:application/octet-stream;base64,' + [Convert]::ToBase64String($Cabinet.Bytes)
[System.IO.File]::WriteAllText((Join-Path $Out 'Speaker_Cabinet.gltf'), ($Cabinet.Document | ConvertTo-Json -Depth 12 -Compress))

# DJ_Booth.glb: body and top plate are separate meshes, so the importer has to combine them into one item.
$Booth = New-GltfDocument @(
    @{ Name = 'BoothBody'; Material = 'M_BoothBody'; Color = @(0.02, 0.02, 0.025, 1.0); Metallic = 0.0; Translation = @(0, 0, 0); Box = (New-Box 2.0 1.0 0.8 0.0) },
    @{ Name = 'BoothTop'; Material = 'M_BoothTop'; Color = @(0.7, 0.7, 0.72, 1.0); Metallic = 1.0; Translation = @(0, 1.0, 0); Box = (New-Box 2.0 0.05 0.8 0.0) }
) $Generator
$Json = [System.Text.Encoding]::UTF8.GetBytes(($Booth.Document | ConvertTo-Json -Depth 12 -Compress))
$JsonPadded = $Json + [byte[]](@(0x20) * ((4 - $Json.Length % 4) % 4))
$Bin = $Booth.Bytes
$BinPadded = $Bin + [byte[]](@(0) * ((4 - $Bin.Length % 4) % 4))
$Glb = New-Object System.IO.MemoryStream
$GlbWriter = New-Object System.IO.BinaryWriter($Glb)
$GlbWriter.Write([uint32]0x46546C67)                                   # "glTF"
$GlbWriter.Write([uint32]2)
$GlbWriter.Write([uint32](12 + 8 + $JsonPadded.Length + 8 + $BinPadded.Length))
$GlbWriter.Write([uint32]$JsonPadded.Length); $GlbWriter.Write([uint32]0x4E4F534A); $GlbWriter.Write([byte[]]$JsonPadded)   # "JSON"
$GlbWriter.Write([uint32]$BinPadded.Length); $GlbWriter.Write([uint32]0x004E4942); $GlbWriter.Write([byte[]]$BinPadded)     # "BIN"
$GlbWriter.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $Out 'DJ_Booth.glb'), $Glb.ToArray())

# Stage_Riser.obj: OBJ has no units or up axis; this one is Y-up in metres like most exporters write it.
$Riser = New-Box 2.0 0.4 1.0 0.0
$Obj = New-Object System.Text.StringBuilder
[void]$Obj.AppendLine('# StageCraft test model: 2 x 1 m stage riser, 0.4 m high (Y-up, metres)')
[void]$Obj.AppendLine('mtllib Stage_Riser.mtl')
[void]$Obj.AppendLine('o StageRiser')
$Culture = [System.Globalization.CultureInfo]::InvariantCulture
for ($Index = 0; $Index -lt $Riser.Positions.Count; $Index += 3) {
    [void]$Obj.AppendLine(('v {0} {1} {2}' -f $Riser.Positions[$Index].ToString($Culture), $Riser.Positions[$Index + 1].ToString($Culture), $Riser.Positions[$Index + 2].ToString($Culture)))
}
for ($Index = 0; $Index -lt $Riser.Normals.Count; $Index += 3) {
    [void]$Obj.AppendLine(('vn {0} {1} {2}' -f $Riser.Normals[$Index], $Riser.Normals[$Index + 1], $Riser.Normals[$Index + 2]))
}
# One texture coordinate per corner (0,0 1,0 1,1 0,1 per face): OBJ without UVs trips an ensure in the engine's OBJ translator.
for ($Index = 0; $Index -lt $Riser.Normals.Count / 3; $Index++) {
    $Corner = @(@(0, 0), @(1, 0), @(1, 1), @(0, 1))[$Index % 4]
    [void]$Obj.AppendLine(("vt {0} {1}" -f $Corner[0], $Corner[1]))
}
[void]$Obj.AppendLine('usemtl M_RiserDeck')
for ($Index = 0; $Index -lt $Riser.Indices.Count; $Index += 3) {
    $A = $Riser.Indices[$Index] + 1; $B = $Riser.Indices[$Index + 1] + 1; $C = $Riser.Indices[$Index + 2] + 1
    [void]$Obj.AppendLine("f $A/$A/$A $B/$B/$B $C/$C/$C")
}
[System.IO.File]::WriteAllText((Join-Path $Out 'Stage_Riser.obj'), $Obj.ToString())
[System.IO.File]::WriteAllText((Join-Path $Out 'Stage_Riser.mtl'), "newmtl M_RiserDeck`nKd 0.12 0.10 0.08`nKs 0.05 0.05 0.05`nNs 20`n")

# Not_A_Model.dae: an extension the app does not import.
[System.IO.File]::WriteAllText((Join-Path $Out 'Not_A_Model.dae'), "<?xml version=`"1.0`"?><COLLADA/>`n")

Get-ChildItem $Out -File | Where-Object { $_.Extension -ne '.ps1' } | ForEach-Object { '{0,-22} {1,8} bytes' -f $_.Name, $_.Length }
