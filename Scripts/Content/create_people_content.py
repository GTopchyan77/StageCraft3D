# Creates the content of Phase I (STATE.md #76, Docs/ADR/0031-stage-people-and-realism.md): the stand-in people's material and the
# audience, band, crew, backline and set items.
#   /Game/StageCraft/People/M_StageFigure        figure material: bone animation texture played in the vertex shader (WPO),
#                                                 colours from per-instance custom data (UStageFigureAssetSubsystem contract)
#   /Game/StageCraft/People/M_Prop*               simple opaque materials for the parts
#   /Game/StageCraft/Items/DA_Audience_Standing   audience area (UStageAudienceData)
#   /Game/StageCraft/Items/DA_Band_*              drummer with kit, guitarist, bassist, vocalist, keyboardist (UStagePropData)
#   /Game/StageCraft/Items/DA_Crew_*              FOH lighting and sound desks with operators, monitor desk, stagehand
#   /Game/StageCraft/Items/DA_Backline_AmpStack   guitar amplifier stack
#   /Game/StageCraft/Items/DA_Set_LEDBackdrop     8 x 4.5 m LED wall on its frame
#   /Game/StageCraft/Items/DA_Span_Barricade      crowd barrier, whole 1 m sections (UStageSpanData)
#
# Every part is an engine basic shape (cube, cylinder) placed, turned and scaled; the people are the procedural figure of
# StageFigureRig, so nothing here needs art. Real art replaces a part's mesh or the figure contract later, as data.
#
# Headless (the project's editor must be closed):
#   UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script="<abs path>/create_people_content.py" -unattended -nullrhi
#
# Idempotent: existing assets are updated in place (the material graph is rebuilt), so references stay valid.

import unreal

PEOPLE_DIR = "/Game/StageCraft/People"
ITEMS_DIR = "/Game/StageCraft/Items"
CUBE = "/Engine/BasicShapes/Cube.Cube"              # 100 cm, pivot at the centre
CYLINDER = "/Engine/BasicShapes/Cylinder.Cylinder"  # 100 cm tall along Z, 100 cm diameter, pivot at the centre
# A linear (not sRGB) default, as the Linear Color sampler requires; the game sets the float animation texture.
LINEAR_TEXTURE = "/Engine/EngineMaterials/BaseFlattenLinearColor.BaseFlattenLinearColor"

mel = unreal.MaterialEditingLibrary
eal = unreal.EditorAssetLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()


def log(message):
    unreal.log("[StageCraftContent] " + message)


def make_tag(name):
    tag = unreal.GameplayTag()
    tag.import_text('(TagName="{}")'.format(name))
    return tag


def load_or_create(name, directory, asset_class, factory):
    path = directory + "/" + name
    if eal.does_asset_exist(path):
        return eal.load_asset(path)
    asset = asset_tools.create_asset(name, directory, asset_class, factory)
    if asset is None:
        raise RuntimeError("Could not create " + path)
    return asset


# --- Figure material ---

# Rotation of this vertex's bone, blended between the two frames around the current time (StageFigureRig.h contract: two texels
# per bone, rotation then translation; rows are frames of clips in order).
POSE_PRELUDE = r"""
float Frames = max(FramesPerClip, 1.0);
float Loop = frac(Time * Rate + Phase);
float F = Loop * Frames;
float F0 = floor(F);
float A = F - F0;
float F1 = (F0 + 1.0 >= Frames) ? 0.0 : F0 + 1.0;
float Base = floor(Clip + 0.5) * Frames;
float V0 = (Base + F0 + 0.5) / TextureRows;
float V1 = (Base + F1 + 0.5) / TextureRows;
float Bone = floor(Figure.x);
float UQ = (Bone * 2.0 + 0.5) / TextureColumns;
float UT = (Bone * 2.0 + 1.5) / TextureColumns;
float4 Q0 = Texture2DSampleLevel(AnimTex, AnimTexSampler, float2(UQ, V0), 0);
float4 Q1 = Texture2DSampleLevel(AnimTex, AnimTexSampler, float2(UQ, V1), 0);
Q1 = dot(Q0, Q1) < 0.0 ? -Q1 : Q1;
float4 Q = normalize(lerp(Q0, Q1, A));
"""

# Local-space offset from the rest pose to the posed vertex: Rotate(q, P) + t - P.
OFFSET_HLSL = POSE_PRELUDE + r"""
float3 T0 = Texture2DSampleLevel(AnimTex, AnimTexSampler, float2(UT, V0), 0).xyz;
float3 T1 = Texture2DSampleLevel(AnimTex, AnimTexSampler, float2(UT, V1), 0).xyz;
float3 T = lerp(T0, T1, A);
float3 Posed = P + 2.0 * cross(Q.xyz, cross(Q.xyz, P) + Q.w * P) + T;
return Posed - P;
"""

# The rest normal turned with its bone (local space).
NORMAL_HLSL = POSE_PRELUDE + r"""
return N + 2.0 * cross(Q.xyz, cross(Q.xyz, N) + Q.w * N);
"""

# Region (UV1.y): 0 skin, 1 shirt, 2 trousers, 3 hair and shoes.
COLOR_HLSL = r"""
float R = Figure.y;
float3 Skin = lerp(float3(0.58, 0.40, 0.30), float3(0.14, 0.08, 0.055), saturate(SkinTone));
float3 Bottom = lerp(float3(0.045, 0.07, 0.14), float3(0.018, 0.018, 0.02), saturate(BottomShade));
float3 Dark = float3(0.012, 0.010, 0.009);
return R < 0.5 ? Skin : (R < 1.5 ? Top : (R < 2.5 ? Bottom : Dark));
"""

POSE_INPUTS = ["AnimTex", "Figure", "Clip", "Phase", "Rate", "Time", "FramesPerClip", "TextureRows", "TextureColumns"]


def add_custom(material, code, inputs, output_type, description, x, y):
    custom = mel.create_material_expression(material, unreal.MaterialExpressionCustom, x, y)
    custom.set_editor_property("code", code)
    custom.set_editor_property("output_type", output_type)
    custom.set_editor_property("description", description)
    custom_inputs = []
    for input_name in inputs:
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", input_name)
        custom_inputs.append(custom_input)
    custom.set_editor_property("inputs", custom_inputs)
    return custom


def instance_data(material, index, x, y):
    node = mel.create_material_expression(material, unreal.MaterialExpressionPerInstanceCustomData, x, y)
    node.set_editor_property("data_index", index)
    return node


def local_to_world(material, x, y):
    node = mel.create_material_expression(material, unreal.MaterialExpressionTransform, x, y)
    node.set_editor_property("transform_source_type", unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_LOCAL)
    node.set_editor_property("transform_type", unreal.MaterialVectorCoordTransform.TRANSFORM_WORLD)
    return node


def create_figure_material():
    material = load_or_create("M_StageFigure", PEOPLE_DIR, unreal.Material, unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(material)
    material.set_editor_property("used_with_instanced_static_meshes", True)
    # The normal output is a world-space normal turned with its bone, not a tangent-space map.
    material.set_editor_property("tangent_space_normal", False)

    texture = mel.create_material_expression(material, unreal.MaterialExpressionTextureObjectParameter, -1400, -200)
    texture.set_editor_property("parameter_name", "AnimTex")
    texture.set_editor_property("texture", unreal.load_asset(LINEAR_TEXTURE))
    texture.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)

    figure = mel.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -1400, -60)
    figure.set_editor_property("coordinate_index", 1)
    clip = instance_data(material, 0, -1400, 20)
    phase = instance_data(material, 1, -1400, 100)
    rate = instance_data(material, 2, -1400, 180)
    time = mel.create_material_expression(material, unreal.MaterialExpressionTime, -1400, 260)
    scalars = {}
    for index, (name, default) in enumerate([("FramesPerClip", 32.0), ("TextureRows", 320.0), ("TextureColumns", 22.0)]):
        scalar = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -1400, 340 + index * 70)
        scalar.set_editor_property("parameter_name", name)
        scalar.set_editor_property("default_value", default)
        scalars[name] = scalar
    position = mel.create_material_expression(material, unreal.MaterialExpressionPreSkinnedPosition, -1400, 560)
    normal = mel.create_material_expression(material, unreal.MaterialExpressionPreSkinnedNormal, -1400, 640)

    def connect_pose(custom):
        mel.connect_material_expressions(texture, "", custom, "AnimTex")
        mel.connect_material_expressions(figure, "", custom, "Figure")
        mel.connect_material_expressions(clip, "", custom, "Clip")
        mel.connect_material_expressions(phase, "", custom, "Phase")
        mel.connect_material_expressions(rate, "", custom, "Rate")
        mel.connect_material_expressions(time, "", custom, "Time")
        for name, scalar in scalars.items():
            mel.connect_material_expressions(scalar, "", custom, name)

    offset = add_custom(material, OFFSET_HLSL, POSE_INPUTS + ["P"], unreal.CustomMaterialOutputType.CMOT_FLOAT3, "Posed offset (local)", -900, 0)
    connect_pose(offset)
    mel.connect_material_expressions(position, "", offset, "P")
    offset_world = local_to_world(material, -500, 0)
    mel.connect_material_expressions(offset, "", offset_world, "")
    mel.connect_material_property(offset_world, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)

    turned = add_custom(material, NORMAL_HLSL, POSE_INPUTS + ["N"], unreal.CustomMaterialOutputType.CMOT_FLOAT3, "Posed normal (local)", -900, 300)
    connect_pose(turned)
    mel.connect_material_expressions(normal, "", turned, "N")
    normal_vs = mel.create_material_expression(material, unreal.MaterialExpressionVertexInterpolator, -650, 300)
    mel.connect_material_expressions(turned, "", normal_vs, "VS")
    normal_world = local_to_world(material, -400, 300)
    mel.connect_material_expressions(normal_vs, "", normal_world, "")
    mel.connect_material_property(normal_world, "", unreal.MaterialProperty.MP_NORMAL)

    top = mel.create_material_expression(material, unreal.MaterialExpressionPerInstanceCustomData3Vector, -1100, -420)
    top.set_editor_property("data_index", 3)
    skin = instance_data(material, 6, -1100, -340)
    bottom = instance_data(material, 7, -1100, -260)
    color = add_custom(material, COLOR_HLSL, ["Figure", "Top", "SkinTone", "BottomShade"], unreal.CustomMaterialOutputType.CMOT_FLOAT3,
        "Region colour", -900, -400)
    mel.connect_material_expressions(figure, "", color, "Figure")
    mel.connect_material_expressions(top, "", color, "Top")
    mel.connect_material_expressions(skin, "", color, "SkinTone")
    mel.connect_material_expressions(bottom, "", color, "BottomShade")
    color_vs = mel.create_material_expression(material, unreal.MaterialExpressionVertexInterpolator, -650, -400)
    mel.connect_material_expressions(color, "", color_vs, "VS")
    mel.connect_material_property(color_vs, "", unreal.MaterialProperty.MP_BASE_COLOR)

    roughness = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -650, -200)
    roughness.set_editor_property("r", 0.8)
    mel.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)
    specular = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -650, -130)
    specular.set_editor_property("r", 0.3)
    mel.connect_material_property(specular, "", unreal.MaterialProperty.MP_SPECULAR)

    mel.recompile_material(material)
    eal.save_loaded_asset(material)

    saved = eal.load_asset(PEOPLE_DIR + "/M_StageFigure")
    if not saved.get_editor_property("used_with_instanced_static_meshes") or saved.get_editor_property("tangent_space_normal"):
        raise RuntimeError("M_StageFigure usage flags did not save")
    log("Saved M_StageFigure ({} expressions)".format(mel.get_num_material_expressions(saved)))
    return saved


# --- Part materials ---

def create_part_material(name, color, metallic, roughness, emissive=None):
    material = load_or_create(name, PEOPLE_DIR, unreal.Material, unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(material)
    material.set_editor_property("used_with_instanced_static_meshes", True)
    base = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -300, 0)
    base.set_editor_property("constant", unreal.LinearColor(color[0], color[1], color[2], 1.0))
    metal = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -300, 150)
    metal.set_editor_property("r", metallic)
    rough = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -300, 250)
    rough.set_editor_property("r", roughness)
    mel.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(metal, "", unreal.MaterialProperty.MP_METALLIC)
    mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    if emissive is not None:
        glow = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -300, 350)
        glow.set_editor_property("constant", unreal.LinearColor(emissive[0], emissive[1], emissive[2], 1.0))
        mel.connect_material_property(glow, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.recompile_material(material)
    eal.save_loaded_asset(material)
    return material


# --- Parts and figures ---

def part(mesh, material, location, rotation=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0)):
    """rotation = (pitch, yaw, roll) in degrees."""
    span_part = unreal.StageSpanPart()
    span_part.set_editor_property("mesh", mesh)
    span_part.set_editor_property("material", material)
    span_part.set_editor_property("transform", unreal.Transform(
        location=unreal.Vector(*location),
        rotation=unreal.Rotator(roll=rotation[2], pitch=rotation[0], yaw=rotation[1]),
        scale=unreal.Vector(*scale)))
    return span_part


class Shapes:
    def __init__(self):
        self.cube = unreal.load_asset(CUBE)
        self.cylinder = unreal.load_asset(CYLINDER)
        if self.cube is None or self.cylinder is None:
            raise RuntimeError("Engine BasicShapes not found")

    def box(self, material, centre, size, rotation=(0.0, 0.0, 0.0)):
        return part(self.cube, material, centre, rotation, (size[0] / 100.0, size[1] / 100.0, size[2] / 100.0))

    def disc(self, material, centre, diameter, height, rotation=(0.0, 0.0, 0.0)):
        """A cylinder standing on Z (turn it with rotation)."""
        return part(self.cylinder, material, centre, rotation, (diameter / 100.0, diameter / 100.0, height / 100.0))


def figure(clip, location, yaw=0.0, top=(0.03, 0.03, 0.035), skin=0.3, bottom=1.0, rate=1.0, phase=0.0):
    placement = unreal.StageFigurePlacement()
    placement.set_editor_property("clip", clip)
    placement.set_editor_property("transform", unreal.Transform(location=unreal.Vector(*location), rotation=unreal.Rotator(0.0, 0.0, yaw)))
    placement.set_editor_property("top_color", unreal.LinearColor(top[0], top[1], top[2], 1.0))
    placement.set_editor_property("skin_tone", skin)
    placement.set_editor_property("bottom_shade", bottom)
    placement.set_editor_property("rate_scale", rate)
    placement.set_editor_property("phase", phase)
    return placement


def make_specs(manufacturer, weight_kg, power_w):
    specs = unreal.StageEquipmentSpecs()
    specs.set_editor_property("manufacturer", manufacturer)
    specs.set_editor_property("weight_kg", weight_kg)
    specs.set_editor_property("power_draw_watts", power_w)
    return specs


def save_prop(name, display_name, description, category, item_type, parts, figures, figure_material, specs, grid=25.0):
    item = load_or_create(name, ITEMS_DIR, unreal.StagePropData, unreal.DataAssetFactory())
    item.set_editor_property("display_name", display_name)
    item.set_editor_property("description", description)
    item.set_editor_property("category_tag", make_tag(category))
    item.set_editor_property("item_type", item_type)
    item.set_editor_property("parts", parts)
    item.set_editor_property("figures", figures)
    item.set_editor_property("figure_material", figure_material if figures else None)
    item.set_editor_property("specs", specs)
    rules = item.get_editor_property("placement_rules")
    rules.set_editor_property("grid_size", unreal.Vector(grid, grid, 0.0))
    item.set_editor_property("placement_rules", rules)
    eal.save_loaded_asset(item)

    saved = eal.load_asset(ITEMS_DIR + "/" + name)
    if len(saved.get_editor_property("parts")) != len(parts) or len(saved.get_editor_property("figures")) != len(figures):
        raise RuntimeError(name + ": parts or figures did not save")
    log("Saved {}: {} parts, {} figures, actor class {}".format(name, len(parts), len(figures), saved.get_editor_property("actor_class")))


Clip = unreal.StageFigureClip
PROP = unreal.StageItemType.PROP
STAGE = unreal.StageItemType.STAGE_ELEMENT
AUDIO = unreal.StageItemType.AUDIO


def wedge(s, mat, x, y=0.0):
    """A floor monitor wedge facing the performer (back towards +X)."""
    return [s.box(mat["black"], (x, y, 18.0), (42.0, 56.0, 30.0), (-28.0, 0.0, 0.0)),
            s.box(mat["grille"], (x - 13.0, y, 22.0), (2.0, 50.0, 26.0), (-28.0, 0.0, 0.0))]


def create_band(s, mat, figure_material):
    # Drummer: kit in front (+X) of a throne, cymbals on stands.
    kit = [
        s.disc(mat["shell"], (62.0, 0.0, 30.0), 56.0, 40.0, (90.0, 0.0, 0.0)),
        s.disc(mat["head"], (83.0, 0.0, 30.0), 54.0, 1.0, (90.0, 0.0, 0.0)),
        s.disc(mat["shell"], (36.0, -24.0, 64.0), 36.0, 15.0),
        s.disc(mat["metal"], (36.0, -24.0, 32.0), 2.5, 58.0),
        s.disc(mat["shell"], (58.0, 12.0, 82.0), 30.0, 22.0, (0.0, 0.0, -15.0)),
        s.disc(mat["shell"], (32.0, 36.0, 40.0), 40.0, 38.0),
        s.disc(mat["brass"], (28.0, -46.0, 96.0), 36.0, 1.0),
        s.disc(mat["brass"], (28.0, -46.0, 93.0), 36.0, 1.0),
        s.disc(mat["metal"], (28.0, -46.0, 47.0), 2.5, 94.0),
        s.disc(mat["brass"], (78.0, -38.0, 140.0), 46.0, 1.0, (0.0, 0.0, 12.0)),
        s.disc(mat["metal"], (78.0, -38.0, 70.0), 2.5, 140.0),
        s.disc(mat["brass"], (74.0, 44.0, 116.0), 52.0, 1.0, (0.0, 0.0, -10.0)),
        s.disc(mat["metal"], (74.0, 44.0, 58.0), 2.5, 116.0),
        s.disc(mat["black"], (-14.0, 0.0, 53.0), 36.0, 7.0),
        s.disc(mat["metal"], (-14.0, 0.0, 25.0), 4.0, 50.0),
    ]
    save_prop("DA_Band_Drummer", "Drummer with Drum Kit",
        "A five-piece drum kit with cymbals and throne, and an animated drummer playing it. Place it on a riser at the back of the stage.",
        "StageCraft.Category.People.Band", PROP, kit,
        [figure(Clip.DRUMMER, (-16.0, 0.0, 0.0), top=(0.02, 0.02, 0.02), skin=0.35)], figure_material, make_specs("Generic", 70.0, 0.0))

    def guitar(neck_length, body):
        return [
            s.box(mat[body], (15.0, 6.0, 101.0), (6.0, 30.0, 38.0), (0.0, 0.0, 28.0)),
            s.box(mat["wood"], (17.0, -28.0, 118.0), (4.0, neck_length, 5.0), (0.0, 0.0, 28.0)),
        ]

    save_prop("DA_Band_Guitarist", "Guitarist",
        "A guitarist strumming and nodding, with a floor wedge in front. Pair with a guitar amplifier stack behind.",
        "StageCraft.Category.People.Band", PROP, guitar(58.0, "shell") + wedge(s, mat, 95.0),
        [figure(Clip.GUITARIST, (0.0, 0.0, 0.0), top=(0.40, 0.03, 0.03), skin=0.15, bottom=0.2)], figure_material, make_specs("Generic", 20.0, 0.0))
    save_prop("DA_Band_Bassist", "Bass Player",
        "A bass player keeping time, with a floor wedge in front.",
        "StageCraft.Category.People.Band", PROP, guitar(76.0, "black") + wedge(s, mat, 95.0),
        [figure(Clip.GUITARIST, (0.0, 0.0, 0.0), top=(0.02, 0.05, 0.12), skin=0.7, bottom=0.9, rate=0.5, phase=0.3)], figure_material,
        make_specs("Generic", 22.0, 0.0))

    stand = [
        s.disc(mat["black"], (42.0, 0.0, 1.0), 30.0, 2.0),
        s.disc(mat["metal"], (42.0, 0.0, 70.0), 2.5, 138.0),
        s.disc(mat["metal"], (36.0, 0.0, 140.0), 3.5, 16.0, (55.0, 0.0, 0.0)),
    ]
    save_prop("DA_Band_Vocalist", "Lead Vocalist",
        "A singer with a hand microphone working the crowd, a microphone stand and a floor wedge.",
        "StageCraft.Category.People.Band", PROP, stand + wedge(s, mat, 105.0),
        [figure(Clip.VOCALIST, (0.0, 0.0, 0.0), top=(0.75, 0.72, 0.68), skin=0.5, bottom=0.95)], figure_material, make_specs("Generic", 18.0, 0.0))

    keys = [
        s.box(mat["metal"], (34.0, -45.0, 44.0), (3.0, 3.0, 96.0), (0.0, 0.0, 30.0)),
        s.box(mat["metal"], (34.0, -45.0, 44.0), (3.0, 3.0, 96.0), (0.0, 0.0, -30.0)),
        s.box(mat["metal"], (34.0, 45.0, 44.0), (3.0, 3.0, 96.0), (0.0, 0.0, 30.0)),
        s.box(mat["metal"], (34.0, 45.0, 44.0), (3.0, 3.0, 96.0), (0.0, 0.0, -30.0)),
        s.box(mat["black"], (36.0, 0.0, 92.0), (32.0, 124.0, 9.0)),
        s.box(mat["keys"], (27.0, 0.0, 96.8), (14.0, 118.0, 1.5)),
    ]
    save_prop("DA_Band_Keyboardist", "Keyboard Player",
        "A keyboard player on an X-stand, nodding along.",
        "StageCraft.Category.People.Band", PROP, keys,
        [figure(Clip.KEYBOARDIST, (0.0, 0.0, 0.0), top=(0.05, 0.09, 0.05), skin=0.25, bottom=0.6, phase=0.2)], figure_material,
        make_specs("Generic", 25.0, 60.0))


def desk(s, mat, width, console_depth, console_width, screens):
    parts = [
        s.box(mat["black"], (45.0, 0.0, 88.0), (80.0, width, 4.0)),
        s.box(mat["black"], (12.0, -width / 2.0 + 6.0, 43.0), (6.0, 6.0, 86.0)),
        s.box(mat["black"], (12.0, width / 2.0 - 6.0, 43.0), (6.0, 6.0, 86.0)),
        s.box(mat["black"], (78.0, -width / 2.0 + 6.0, 43.0), (6.0, 6.0, 86.0)),
        s.box(mat["black"], (78.0, width / 2.0 - 6.0, 43.0), (6.0, 6.0, 86.0)),
        s.box(mat["console"], (40.0, 0.0, 95.0), (console_depth, console_width, 10.0), (-8.0, 0.0, 0.0)),
    ]
    for y in screens:
        parts.append(s.box(mat["black"], (72.0, y, 122.0), (4.0, 56.0, 36.0), (-12.0, 0.0, 0.0)))
        parts.append(s.box(mat["screen"], (69.8, y, 122.4), (0.6, 52.0, 32.0), (-12.0, 0.0, 0.0)))
    return parts


def create_crew(s, mat, figure_material):
    save_prop("DA_Crew_LightingDesk", "FOH Lighting Position (desk + operator)",
        "Front-of-house lighting desk with a console and two screens, and an operator watching the stage. Face it towards the stage.",
        "StageCraft.Category.People.Crew", PROP, desk(s, mat, 200.0, 56.0, 110.0, [-34.0, 34.0]),
        [figure(Clip.DESK_OPERATOR, (-22.0, 0.0, 0.0), top=(0.02, 0.02, 0.02), skin=0.2)], figure_material, make_specs("Generic", 45.0, 600.0))
    save_prop("DA_Crew_SoundDesk", "FOH Sound Position (desk + engineer)",
        "Front-of-house mixing desk with one screen, and a sound engineer at it. Face it towards the stage.",
        "StageCraft.Category.People.Crew", PROP, desk(s, mat, 220.0, 64.0, 160.0, [0.0]),
        [figure(Clip.DESK_OPERATOR, (-22.0, 0.0, 0.0), top=(0.10, 0.10, 0.11), skin=0.55, bottom=0.4, phase=0.45, rate=0.8)], figure_material,
        make_specs("Generic", 60.0, 800.0))
    save_prop("DA_Crew_MonitorDesk", "Monitor Position (side stage)",
        "Monitor mixing desk for the wings, with its engineer.",
        "StageCraft.Category.People.Crew", PROP, desk(s, mat, 140.0, 50.0, 100.0, [0.0]),
        [figure(Clip.DESK_OPERATOR, (-22.0, 0.0, 0.0), top=(0.02, 0.02, 0.02), skin=0.8, phase=0.7, rate=1.2)], figure_material,
        make_specs("Generic", 40.0, 500.0))
    save_prop("DA_Crew_Stagehand", "Stagehand with Road Case",
        "A stagehand standing by in the wings next to a road case.",
        "StageCraft.Category.People.Crew", PROP,
        [s.box(mat["black"], (-5.0, 70.0, 33.0), (60.0, 80.0, 62.0)), s.box(mat["metal"], (-5.0, 70.0, 64.5), (62.0, 82.0, 1.5))],
        [figure(Clip.STAGEHAND, (0.0, 0.0, 0.0), top=(0.02, 0.02, 0.02), skin=0.4)], figure_material, make_specs("Generic", 40.0, 0.0))


def create_backline_and_set(s, mat):
    cabinet = lambda z: [s.box(mat["black"], (0.0, 0.0, z), (36.0, 76.0, 76.0)), s.box(mat["grille"], (18.5, 0.0, z), (1.0, 68.0, 68.0))]
    save_prop("DA_Backline_AmpStack", "Guitar Amplifier Stack (2 x 4x12 + head)",
        "Two 4x12 speaker cabinets with an amplifier head on top.",
        "StageCraft.Category.Prop.Backline", AUDIO,
        cabinet(38.0) + cabinet(115.0) + [s.box(mat["black"], (0.0, 0.0, 167.0), (28.0, 76.0, 26.0)),
                                          s.box(mat["metal"], (14.5, 0.0, 167.0), (1.0, 70.0, 8.0))],
        [], None, make_specs("Generic", 95.0, 1500.0))

    width, height, bottom = 800.0, 450.0, 120.0
    save_prop("DA_Set_LEDBackdrop", "LED Wall Backdrop (8 x 4.5 m)",
        "An 8 by 4.5 m LED wall on a ground-supported frame, for the back of the stage. Weight and power are for the whole wall.",
        "StageCraft.Category.Stage.Set", STAGE,
        [s.box(mat["metal"], (0.0, -width / 2.0 - 15.0, (bottom + height) / 2.0 + 20.0), (20.0, 20.0, bottom + height + 40.0)),
         s.box(mat["metal"], (0.0, width / 2.0 + 15.0, (bottom + height) / 2.0 + 20.0), (20.0, 20.0, bottom + height + 40.0)),
         s.box(mat["metal"], (0.0, 0.0, bottom + height + 30.0), (20.0, width + 50.0, 20.0)),
         s.box(mat["metal"], (-60.0, -width / 2.0 - 15.0, 2.0), (140.0, 40.0, 4.0)),
         s.box(mat["metal"], (-60.0, width / 2.0 + 15.0, 2.0), (140.0, 40.0, 4.0)),
         s.box(mat["black"], (0.0, 0.0, bottom + height / 2.0), (14.0, width, height)),
         s.box(mat["led"], (7.2, 0.0, bottom + height / 2.0), (0.6, width - 4.0, height - 4.0))],
        [], None, make_specs("Generic", 1100.0, 7200.0), grid=50.0)


def create_barricade(s, mat):
    # Crowd barrier sections along +X: the front panel faces the crowd on +Y, which stands on the base plate.
    segment = 100.0
    parts = [
        s.box(mat["galv"], (segment / 2.0, 0.0, 120.0), (segment, 5.0, 5.0)),
        s.box(mat["galv"], (segment / 2.0, 0.0, 66.0), (segment, 2.5, 100.0)),
        s.box(mat["galv"], (segment / 2.0, 46.0, 1.0), (segment, 92.0, 2.0)),
        s.box(mat["galv"], (segment / 2.0, -38.0, 1.5), (segment, 6.0, 3.0)),
    ]
    for x in (8.0, segment - 8.0):
        parts.append(s.box(mat["galv"], (x, -19.0, 60.0), (4.0, 4.0, 128.0), (0.0, 0.0, -18.0)))
    item = load_or_create("DA_Span_Barricade", ITEMS_DIR, unreal.StageSpanData, unreal.DataAssetFactory())
    item.set_editor_property("display_name", "Crowd Barrier (1 m sections)")
    item.set_editor_property("description", "Front-of-stage crowd barrier drawn from a start to an end point, in whole 1 m sections. The crowd side has the base plate.")
    item.set_editor_property("category_tag", make_tag("StageCraft.Category.Stage.Railing"))
    item.set_editor_property("item_type", STAGE)
    item.set_editor_property("fit", unreal.StageSpanFit.WHOLE_SEGMENTS)
    item.set_editor_property("segment_length", segment)
    item.set_editor_property("max_segments", 300)
    item.set_editor_property("angle_snap_degrees", 15.0)
    item.set_editor_property("segment_parts", parts)
    item.set_editor_property("joint_parts", [])
    item.set_editor_property("specs", make_specs("Generic", 38.0, 0.0))
    rules = item.get_editor_property("placement_rules")
    rules.set_editor_property("grid_size", unreal.Vector(50.0, 50.0, 0.0))
    item.set_editor_property("placement_rules", rules)
    eal.save_loaded_asset(item)
    saved = eal.load_asset(ITEMS_DIR + "/DA_Span_Barricade")
    log("Saved DA_Span_Barricade: {} segment parts".format(len(saved.get_editor_property("segment_parts"))))


def create_audience(figure_material):
    item = load_or_create("DA_Audience_Standing", ITEMS_DIR, unreal.StageAudienceData, unreal.DataAssetFactory())
    item.set_editor_property("display_name", "Audience Area (standing)")
    item.set_editor_property("description", "A standing crowd filling a rectangle of floor: set its width, depth, density (people per square metre) and mood. "
        "Its front edge is where you place it; turn it to face the stage.")
    item.set_editor_property("category_tag", make_tag("StageCraft.Category.People.Audience"))
    item.set_editor_property("item_type", PROP)
    item.set_editor_property("default_size", unreal.Vector2D(1600.0, 1000.0))
    item.set_editor_property("min_size", 200.0)
    item.set_editor_property("max_size", 8000.0)
    item.set_editor_property("default_density", 2.0)
    item.set_editor_property("min_density", 0.25)
    item.set_editor_property("max_density", 4.0)
    item.set_editor_property("max_people", 20000)
    item.set_editor_property("default_mood", unreal.StageCrowdMood.DANCING)
    # A concert crowd: mostly black and dark clothes, some white, a few colours.
    palette = [(0.015, 0.015, 0.016)] * 6 + [(0.05, 0.05, 0.055)] * 3 + [(0.62, 0.62, 0.60)] * 2 + [
        (0.02, 0.03, 0.09), (0.30, 0.025, 0.025), (0.07, 0.08, 0.035), (0.09, 0.15, 0.26), (0.55, 0.40, 0.05), (0.50, 0.15, 0.25)]
    item.set_editor_property("top_colors", [unreal.LinearColor(r, g, b, 1.0) for (r, g, b) in palette])
    item.set_editor_property("figure_material", figure_material)
    item.set_editor_property("cull_distance", 30000.0)
    item.set_editor_property("specs", make_specs("", 0.0, 0.0))
    rules = item.get_editor_property("placement_rules")
    rules.set_editor_property("grid_size", unreal.Vector(50.0, 50.0, 0.0))
    item.set_editor_property("placement_rules", rules)
    eal.save_loaded_asset(item)
    saved = eal.load_asset(ITEMS_DIR + "/DA_Audience_Standing")
    log("Saved DA_Audience_Standing: actor class {}, {} colours".format(saved.get_editor_property("actor_class"), len(saved.get_editor_property("top_colors"))))


figure_material = create_figure_material()
materials = {
    "black": create_part_material("M_PropBlack", (0.012, 0.012, 0.013), 0.0, 0.55),
    "grille": create_part_material("M_PropGrille", (0.025, 0.025, 0.025), 0.0, 0.95),
    "metal": create_part_material("M_PropMetal", (0.55, 0.56, 0.58), 1.0, 0.35),
    "galv": create_part_material("M_PropGalvanised", (0.42, 0.43, 0.44), 1.0, 0.55),
    "brass": create_part_material("M_PropBrass", (0.75, 0.52, 0.18), 1.0, 0.3),
    "shell": create_part_material("M_PropDrumShell", (0.22, 0.02, 0.025), 0.2, 0.25),
    "head": create_part_material("M_PropDrumHead", (0.70, 0.68, 0.62), 0.0, 0.6),
    "wood": create_part_material("M_PropWood", (0.20, 0.11, 0.05), 0.0, 0.5),
    "keys": create_part_material("M_PropKeys", (0.80, 0.80, 0.78), 0.0, 0.4),
    "console": create_part_material("M_PropConsole", (0.04, 0.04, 0.045), 0.3, 0.45),
    "screen": create_part_material("M_PropScreen", (0.01, 0.01, 0.012), 0.0, 0.2, emissive=(0.05, 0.12, 0.25)),
    "led": create_part_material("M_PropLEDWall", (0.004, 0.004, 0.005), 0.0, 0.15, emissive=(0.006, 0.009, 0.024)),
}
shapes = Shapes()
create_audience(figure_material)
create_band(shapes, materials, figure_material)
create_crew(shapes, materials, figure_material)
create_backline_and_set(shapes, materials)
create_barricade(shapes, materials)
log("Done.")
