# Creates the viewport's reference grid material (STATE.md #48, Docs/ADR/0008-viewport-toolbar-projections-and-view-modes.md §4.4):
#   /Game/StageCraft/Viewport/M_StageGrid    translucent, unlit, drawn on one quad by AStageGridActor
#
# Why a material and not line primitives: the old grid drew 1-pixel lines that were either fully on or fully off in a pixel.
# Under TSR's sub-pixel jitter every line pixel toggled from frame to frame, which TSR rejects as flicker, so the whole grid
# crawled (measured: 7 % of the viewport above a luma deviation of 4, 0.24 % without the grid). Here each line's coverage is
# computed analytically from the screen-space derivative of the plane coordinate (the method of the Unreal Editor's own grid),
# so a line is a smooth 1-pixel ramp that TSR resolves. Each line family fades out once its cells shrink below a few pixels,
# which removes the moire toward the horizon.
#
# The plane's two in-plane axes come in as world vectors (AxisU, AxisV), so one material serves the floor and the vertical
# grids of the front and side views. The colour of the axis line running along U / V comes in as ColorU / ColorV.
# A small world-position offset toward the camera keeps the grid in front of a floor it lies on (no z-fighting).
#
# Headless (the project's editor must be closed):
#   UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script="<abs path>/create_grid_content.py" -unattended -nullrhi
#
# Idempotent: the graph is rebuilt from scratch on every run.

import unreal

VIEWPORT_DIR = "/Game/StageCraft/Viewport"
MATERIAL_NAME = "M_StageGrid"

GRID_HLSL = r"""
float2 P = float2(dot(WorldPos, AxisU.xyz), dot(WorldPos, AxisV.xyz));

// Coverage of the nearest line of each direction of a family with cells of Period, as a ramp one LineWidth pixels wide.
// The lines running along U (constant V) fade with the V derivative, so at a grazing angle only the compressed direction fades.
float2 Minor = 0;
float2 Major = 0;
{
    float2 C = P / max(CellCm, 0.01);
    float2 W = max(fwidth(C), 1e-6);
    float2 D = abs(frac(C + 0.5) - 0.5) / W;
    Minor = saturate(LineWidth * 0.5 + 0.5 - D) * (1 - smoothstep(FadeStart, FadeEnd, W));
}
if (MajorEvery > 0.5)
{
    float2 C = P / max(CellCm * MajorEvery, 0.01);
    float2 W = max(fwidth(C), 1e-6);
    float2 D = abs(frac(C + 0.5) - 0.5) / W;
    Major = saturate(LineWidth * 0.5 + 0.5 - D) * (1 - smoothstep(FadeStart, FadeEnd, W));
}

// The axis lines through the origin: the one along U lies where V = 0.
float2 AxisDistance = abs(P) / max(fwidth(P), 1e-6);
float AlongU = saturate(AxisWidth * 0.5 + 0.5 - AxisDistance.y);
float AlongV = saturate(AxisWidth * 0.5 + 0.5 - AxisDistance.x);

float3 Colour = MinorColor.rgb;
float Alpha = max(Minor.x, Minor.y);
float MajorAlpha = max(Major.x, Major.y);
Colour = lerp(Colour, MajorColor.rgb, MajorAlpha);
Alpha = max(Alpha, MajorAlpha);
Colour = lerp(Colour, ColorU.rgb, AlongU);
Alpha = max(Alpha, AlongU);
Colour = lerp(Colour, ColorV.rgb, AlongV);
Alpha = max(Alpha, AlongV);
return float4(Colour, Alpha * Opacity);
"""

# Pulls each vertex toward the camera by a distance-proportional amount: in front of a coplanar floor at any range.
PULL_HLSL = r"""
float3 ToCamera = CameraPos - WorldPos;
float Distance = length(ToCamera);
return ToCamera / max(Distance, 1.0) * (Distance * PullRatio + PullMin);
"""

GRID_SCALARS = [
    ("CellCm", 50.0),      # the grid snap; set by AStageGridActor
    ("MajorEvery", 10.0),  # a major line every this many cells (0 = none); set by AStageGridActor
    ("LineWidth", 1.0),    # pixels
    ("AxisWidth", 1.5),    # pixels
    ("FadeStart", 0.12),   # cells per pixel at which a line family starts to fade (about 8 px cells)
    ("FadeEnd", 0.30),     # and is gone (about 3 px cells)
    ("Opacity", 1.0),
]

GRID_VECTORS = [
    ("AxisU", unreal.LinearColor(1.0, 0.0, 0.0, 0.0)),
    ("AxisV", unreal.LinearColor(0.0, 1.0, 0.0, 0.0)),
    ("MinorColor", unreal.LinearColor(0.26, 0.27, 0.29, 1.0)),
    ("MajorColor", unreal.LinearColor(0.42, 0.43, 0.46, 1.0)),
    ("ColorU", unreal.LinearColor(0.60, 0.12, 0.10, 1.0)),
    ("ColorV", unreal.LinearColor(0.15, 0.50, 0.12, 1.0)),
]

PULL_SCALARS = [
    ("PullRatio", 0.002),  # 2 mm per metre of distance
    ("PullMin", 0.5),      # cm
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


def make_custom(material, code, output_type, description, input_names, x, y):
    custom = mel.create_material_expression(material, unreal.MaterialExpressionCustom, x, y)
    custom.set_editor_property("code", code)
    custom.set_editor_property("output_type", output_type)
    custom.set_editor_property("description", description)
    inputs = []
    for input_name in input_names:
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", input_name)
        inputs.append(custom_input)
    custom.set_editor_property("inputs", inputs)
    return custom


def make_mask(material, channels, x, y):
    mask = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, x, y)
    for channel in ("r", "g", "b", "a"):
        mask.set_editor_property(channel, channel in channels)
    return mask


def mask_channels(mask):
    return tuple(channel for channel in ("r", "g", "b", "a") if mask.get_editor_property(channel))


def connect_parameters(material, custom, scalars, vectors, x, y):
    for name, default in scalars:
        scalar = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, x, y)
        scalar.set_editor_property("parameter_name", name)
        scalar.set_editor_property("default_value", default)
        mel.connect_material_expressions(scalar, "", custom, name)
        y += 60
    for name, default in vectors:
        vector = mel.create_material_expression(material, unreal.MaterialExpressionVectorParameter, x, y)
        vector.set_editor_property("parameter_name", name)
        vector.set_editor_property("default_value", default)
        mel.connect_material_expressions(vector, "", custom, name)
        y += 120
    return y


def create_grid_material():
    material = load_or_create(MATERIAL_NAME, VIEWPORT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(material)
    material.set_editor_property("material_domain", unreal.MaterialDomain.MD_SURFACE)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("two_sided", True)
    # One quad spans 100 m: fog evaluated per vertex would be wrong across it.
    material.set_editor_property("compute_fog_per_pixel", True)

    grid_inputs = ["WorldPos"] + [name for name, _ in GRID_SCALARS] + [name for name, _ in GRID_VECTORS]
    grid = make_custom(material, GRID_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT4, "Anti-aliased grid", grid_inputs, -300, 0)
    world_position = mel.create_material_expression(material, unreal.MaterialExpressionWorldPosition, -700, -200)
    world_position.set_editor_property("world_position_shader_offset", unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    mel.connect_material_expressions(world_position, "", grid, "WorldPos")
    connect_parameters(material, grid, GRID_SCALARS, GRID_VECTORS, -700, -100)

    # Every channel is set explicitly: a new ComponentMask starts with R and G already on.
    rgb = make_mask(material, ("r", "g", "b"), 0, -50)
    mel.connect_material_expressions(grid, "", rgb, "")
    alpha = make_mask(material, ("a",), 0, 50)
    mel.connect_material_expressions(grid, "", alpha, "")
    # Emissive is scaled by the view's exposure; undo it so the line colours read the same in a dark night scene as in
    # daylight, as the old line primitives did (without this the grid glowed white once auto exposure brightened a dark stage).
    exposure = mel.create_material_expression(material, unreal.MaterialExpressionEyeAdaptationInverse, 200, -50)
    if not any(mel.connect_material_expressions(rgb, "", exposure, pin) for pin in ("LightValue", "LightValueInput")):
        raise RuntimeError("Could not connect the grid colour to EyeAdaptationInverse")
    mel.connect_material_property(exposure, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.connect_material_property(alpha, "", unreal.MaterialProperty.MP_OPACITY)

    pull = make_custom(material, PULL_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT3, "Pull toward the camera",
                       ["WorldPos", "CameraPos"] + [name for name, _ in PULL_SCALARS], -300, 900)
    pull_position = mel.create_material_expression(material, unreal.MaterialExpressionWorldPosition, -700, 800)
    pull_position.set_editor_property("world_position_shader_offset", unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    mel.connect_material_expressions(pull_position, "", pull, "WorldPos")
    camera_position = mel.create_material_expression(material, unreal.MaterialExpressionCameraPositionWS, -700, 880)
    mel.connect_material_expressions(camera_position, "", pull, "CameraPos")
    connect_parameters(material, pull, PULL_SCALARS, [], -700, 960)
    mel.connect_material_property(pull, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)

    mel.recompile_material(material)
    if not eal.save_loaded_asset(material):
        raise RuntimeError("Could not save " + VIEWPORT_DIR + "/" + MATERIAL_NAME)

    # Read back: a wrong blend mode or shading model would draw an opaque sheet over the stage.
    saved = eal.load_asset(VIEWPORT_DIR + "/" + MATERIAL_NAME)
    blend = saved.get_editor_property("blend_mode")
    shading = saved.get_editor_property("shading_model")
    if blend != unreal.BlendMode.BLEND_TRANSLUCENT or shading != unreal.MaterialShadingModel.MSM_UNLIT or not saved.get_editor_property("two_sided"):
        raise RuntimeError("{}: saved as {} / {}".format(MATERIAL_NAME, blend, shading))
    # And opacity must be the coverage alone: a mask that also passes red makes the whole quad a grey sheet.
    opacity = mel.get_material_property_input_node(saved, unreal.MaterialProperty.MP_OPACITY)
    emissive = mel.get_material_property_input_node(saved, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    if mask_channels(opacity) != ("a",) or not isinstance(emissive, unreal.MaterialExpressionEyeAdaptationInverse):
        raise RuntimeError("{}: opacity reads {}, emissive from {}".format(MATERIAL_NAME, mask_channels(opacity), emissive.get_class().get_name()))
    log("Saved {}/{} ({}, {}, {} expressions)".format(VIEWPORT_DIR, MATERIAL_NAME, blend, shading, mel.get_num_material_expressions(saved)))


create_grid_material()
log("Done.")
