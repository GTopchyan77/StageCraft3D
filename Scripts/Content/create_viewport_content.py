# Creates the viewport's CAD line-view material (Docs/ADR/0008-viewport-toolbar-projections-and-view-modes.md §4.3):
#   /Game/StageCraft/Viewport/M_StageLineView    post-process, after tonemapping
#
# Silhouettes and hard edges are found in screen space and drawn over a flat background:
#   - depth discontinuities: the Laplacian of an affine depth (linear depth in ortho, inverse depth in perspective), so a
#     flat floor seen at a grazing angle is not an edge; only the nearer side of a step is marked, for 1-pixel lines;
#   - creases: neighbouring GBuffer world normals that differ by more than NormalCos;
#   - the scene colour is added back: in Lines mode it holds only the grid lines and emissive surfaces.
# UStageCraftGameViewportClient::ApplyStageViewMode turns lighting, AA jitter and eye adaptation off for this mode.
#
# Headless (the project's editor must be closed):
#   UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script="<abs path>/create_viewport_content.py" -unattended -nullrhi
#
# Idempotent: the graph is rebuilt from scratch on every run.

import unreal

VIEWPORT_DIR = "/Game/StageCraft/Viewport"
MATERIAL_NAME = "M_StageLineView"

LINE_VIEW_HLSL = r"""
float2 UV = GetDefaultSceneTextureUV(Parameters, PPI_SceneDepth);
float2 Texel = View.BufferSizeAndInvSize.zw;
// ViewToClip[3][3] is 1 for an orthographic projection and 0 for a perspective one.
bool bOrtho = View.ViewToClip[3][3] > 0.5;

float DC = SceneTextureLookup(UV, PPI_SceneDepth, false).r;
float3 NC = SceneTextureLookup(UV, PPI_WorldNormal, false).rgb;
float3 SceneC = SceneTextureLookup(UV, PPI_PostProcessInput0, false).rgb;

float Edge = 0;
if (DC < SkyDepth)
{
    float2 Offsets[2] = { float2(Texel.x, 0), float2(0, Texel.y) };
    UNROLL
    for (int i = 0; i < 2; i++)
    {
        float DA = SceneTextureLookup(UV + Offsets[i], PPI_SceneDepth, false).r;
        float DB = SceneTextureLookup(UV - Offsets[i], PPI_SceneDepth, false).r;
        if (DA >= SkyDepth || DB >= SkyDepth)
        {
            Edge = 1;  // Silhouette against the empty background.
            continue;
        }

        // Only the nearer side of a depth step draws, so outlines stay one pixel wide.
        if (0.5 * (DA + DB) > DC)
        {
            float Ratio = bOrtho
                ? abs(DA + DB - 2 * DC) / OrthoDepthStep
                : abs(1 / DA + 1 / DB - 2 / DC) * DC / PerspDepthRatio;
            Edge = max(Edge, saturate(Ratio - 1));
        }

        float3 NA = SceneTextureLookup(UV + Offsets[i], PPI_WorldNormal, false).rgb;
        float3 NB = SceneTextureLookup(UV - Offsets[i], PPI_WorldNormal, false).rgb;
        float Crease = min(dot(NC, NA), dot(NC, NB));
        Edge = max(Edge, saturate((NormalCos - Crease) * 20));
    }
}

// The scene colour keeps the grid lines and emissive surfaces on top of the drawing.
return lerp(Background.rgb, LineColor.rgb, Edge) + SceneC;
"""

SCALARS = [
    ("OrthoDepthStep", 4.0),     # cm: a depth step of this much (Laplacian) starts a line in orthographic views
    ("PerspDepthRatio", 0.006),  # relative inverse-depth change that starts a line in perspective
    ("NormalCos", 0.85),         # neighbours whose normals differ by more than ~32 degrees make a crease
    ("SkyDepth", 1000000.0),     # cm: farther than this is empty background
]

VECTORS = [
    ("Background", unreal.LinearColor(0.045, 0.047, 0.053, 1.0)),
    ("LineColor", unreal.LinearColor(0.80, 0.82, 0.86, 1.0)),
]

SCENE_TEXTURES = [
    ("DepthTex", unreal.SceneTextureId.PPI_SCENE_DEPTH),
    ("NormalTex", unreal.SceneTextureId.PPI_WORLD_NORMAL),
    ("SceneTex", unreal.SceneTextureId.PPI_POST_PROCESS_INPUT0),
]

mel = unreal.MaterialEditingLibrary
eal = unreal.EditorAssetLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()


def log(message):
    unreal.log("[StageCraftContent] " + message)


def load_or_create(name, directory, asset_class, factory):
    path = directory + "/" + name
    if eal.does_asset_exist(path):
        return eal.load_asset(path)
    asset = asset_tools.create_asset(name, directory, asset_class, factory)
    if asset is None:
        raise RuntimeError("Could not create " + path)
    return asset


def create_line_view_material():
    material = load_or_create(MATERIAL_NAME, VIEWPORT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(material)
    material.set_editor_property("material_domain", unreal.MaterialDomain.MD_POST_PROCESS)
    material.set_editor_property("blendable_location", unreal.BlendableLocation.BL_SCENE_COLOR_AFTER_TONEMAPPING)

    custom = mel.create_material_expression(material, unreal.MaterialExpressionCustom, -200, 0)
    custom.set_editor_property("code", LINE_VIEW_HLSL)
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    custom.set_editor_property("description", "CAD line view")
    input_names = [name for name, _ in SCENE_TEXTURES] + [name for name, _ in SCALARS] + [name for name, _ in VECTORS]
    inputs = []
    for input_name in input_names:
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", input_name)
        inputs.append(custom_input)
    custom.set_editor_property("inputs", inputs)

    # Connected scene textures make the material declare its use of them; the code samples them itself at neighbouring texels.
    y = -300
    for name, texture_id in SCENE_TEXTURES:
        scene_texture = mel.create_material_expression(material, unreal.MaterialExpressionSceneTexture, -600, y)
        scene_texture.set_editor_property("scene_texture_id", texture_id)
        mel.connect_material_expressions(scene_texture, "Color", custom, name)
        y += 100

    for name, default in SCALARS:
        scalar = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -600, y)
        scalar.set_editor_property("parameter_name", name)
        scalar.set_editor_property("default_value", default)
        mel.connect_material_expressions(scalar, "", custom, name)
        y += 60

    for name, default in VECTORS:
        vector = mel.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -600, y)
        vector.set_editor_property("parameter_name", name)
        vector.set_editor_property("default_value", default)
        mel.connect_material_expressions(vector, "", custom, name)
        y += 120

    mel.connect_material_property(custom, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.recompile_material(material)
    if not eal.save_loaded_asset(material):
        raise RuntimeError("Could not save " + VIEWPORT_DIR + "/" + MATERIAL_NAME)

    # Read back: a wrong domain or location would silently do nothing in the viewport.
    saved = eal.load_asset(VIEWPORT_DIR + "/" + MATERIAL_NAME)
    domain = saved.get_editor_property("material_domain")
    location = saved.get_editor_property("blendable_location")
    if domain != unreal.MaterialDomain.MD_POST_PROCESS or location != unreal.BlendableLocation.BL_SCENE_COLOR_AFTER_TONEMAPPING:
        raise RuntimeError("{}: saved as {} / {}".format(MATERIAL_NAME, domain, location))
    log("Saved {}/{} ({}, {}, {} expressions)".format(VIEWPORT_DIR, MATERIAL_NAME, domain, location, mel.get_num_material_expressions(saved)))


create_line_view_material()
log("Done.")
