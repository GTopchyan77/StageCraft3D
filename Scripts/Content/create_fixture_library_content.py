# Creates the professional fixture suite content (STATE.md #49, Docs/ADR/0020-professional-fixture-suite.md):
#   /Game/StageCraft/Lighting/M_StageBeamLightFunction    light function: two gobo wheels (index + spin), iris, blades or barndoors,
#                                                          prism copies (circular / linear, spinning), frost, field gate
#   /Game/StageCraft/Lighting/M_StageFixtureEmitter       unlit lens / pixel-cell glow (EmitterColor or CellTexture, Level, Gate)
#   /Game/StageCraft/Lighting/DT_StageGels                gel library, filled from Scripts/Content/Data/StageGels.csv
#   /Game/StageCraft/Lighting/FixtureMeshes/SM_FixtureBase_*   base meshes built at each model's real size (box collision)
#   /Game/StageCraft/Fixtures/DA_Fixture_*                eleven generic fixture models with dimensions, photometrics and DMX modes
#
# Headless (no editor window needed; this project's editor must be closed):
#   UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script="<abs path>/create_fixture_library_content.py" -unattended -nullrhi
#
# Idempotent: assets are rebuilt or updated in place, so references stay valid. Every asset is read back and checked.

import os
import unreal

LIGHTING_DIR = "/Game/StageCraft/Lighting"
GOBO_DIR = LIGHTING_DIR + "/Gobos"
MESH_DIR = LIGHTING_DIR + "/FixtureMeshes"
FIXTURE_DIR = "/Game/StageCraft/Fixtures"
WHITE_TEXTURE = "/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"
CUBE = "/Engine/BasicShapes/Cube.Cube"
CYLINDER = "/Engine/BasicShapes/Cylinder.Cylinder"
GEL_CSV = os.path.join(os.path.dirname(os.path.abspath(__file__)), "Data", "StageGels.csv")

mel = unreal.MaterialEditingLibrary
eal = unreal.EditorAssetLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()


def log(message):
    unreal.log("[StageCraftContent] " + message)


def make_tag(name):
    tag = unreal.GameplayTag()
    tag.import_text('(TagName="{}")'.format(name))
    return tag


def enum_value(enum_class, *names):
    # UE's Python names for CamelCase enumerators vary (LEDPar -> LED_PAR or LED_PAR?); try the spellings in order.
    for name in names:
        if hasattr(enum_class, name):
            return getattr(enum_class, name)
    raise RuntimeError("{} has none of {}".format(enum_class, names))


def load_or_create(name, directory, asset_class, factory):
    path = directory + "/" + name
    if eal.does_asset_exist(path):
        return eal.load_asset(path)
    asset = asset_tools.create_asset(name, directory, asset_class, factory)
    if asset is None:
        raise RuntimeError("Could not create " + path)
    return asset


def add_custom(material, code, inputs, output_type, description, x=-300, y=0):
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


def add_texture_object(material, custom, parameter, pin, sampler_type, y):
    texture = mel.create_material_expression(material, unreal.MaterialExpressionTextureObjectParameter, -800, y)
    texture.set_editor_property("parameter_name", parameter)
    texture.set_editor_property("texture", unreal.load_asset(WHITE_TEXTURE))
    texture.set_editor_property("sampler_type", sampler_type)
    mel.connect_material_expressions(texture, "", custom, pin)


def add_scalars(material, custom, scalars, y):
    for name, default in scalars:
        scalar = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -800, y)
        scalar.set_editor_property("parameter_name", name)
        scalar.set_editor_property("default_value", default)
        mel.connect_material_expressions(scalar, "", custom, name)
        y += 60
    return y


# --- Beam-shaping light function ---

# Cone-space mask. UV runs 0..1 across the spot cone (LightFunctionCommon.ush). ConeToBeam maps it so the beam edge (gobo,
# iris and gate) sits at radius 1 and the wider field and prism copies fit inside the cone. Spin angles run on the material's
# Time (world game time) from the phase and start time the fixture set when the speed last changed, so nothing ticks.
BEAM_SHAPING_HLSL = r"""
float2 p = (UV - 0.5) * 2.0 * ConeToBeam;
float Frosted = saturate(Frost);
float Soft = lerp(0.015, 0.3, Frosted);
float BladeSoft = max(Soft, BladeSoftness);
float Blur = Frosted * 5.0;

float GoboAngle = radians(GoboRotation + GoboSpinPhase + GoboSpinRpm * 6.0 * max(Time - GoboSpinStart, 0.0));
float PrismAngle = radians(PrismSpinPhase + PrismSpinRpm * 6.0 * max(Time - PrismSpinStart, 0.0));
float BladeAngle = radians(BladeRotation);
float cg = cos(GoboAngle), sg = sin(GoboAngle);
float cp = cos(PrismAngle), sp = sin(PrismAngle);
float cb = cos(BladeAngle), sb = sin(BladeAngle);

int Copies = PrismFacets > 1.5 ? (int)(PrismFacets + 0.5) : 1;
float Mask = 0.0;
[loop] for (int i = 0; i < Copies; ++i)
{
    float2 c = float2(0.0, 0.0);
    if (Copies > 1)
    {
        if (PrismLinear > 0.5)
        {
            c = float2((i - (Copies - 1) * 0.5) * PrismSpread, 0.0);
        }
        else
        {
            float a = 6.2831853 * i / Copies;
            c = float2(cos(a), sin(a)) * PrismSpread;
        }
        c = float2(cp * c.x - sp * c.y, sp * c.x + cp * c.y);
    }
    float2 q = p - c;
    float r = length(q);

    float Gate = 1.0 - smoothstep(1.0 - Soft, 1.0 + Soft, r);

    float Gobo = 1.0;
    if (GoboEnabled > 0.5)
    {
        float2 g = float2(cg * q.x - sg * q.y, sg * q.x + cg * q.y) * 0.5 + 0.5;
        float Inside = (all(g >= 0.0) && all(g <= 1.0)) ? 1.0 : 0.0;
        Gobo = Texture2DSampleBias(GoboTex, GoboTexSampler, saturate(g), Blur).r * Inside;
    }
    float Gobo2 = 1.0;
    if (Gobo2Enabled > 0.5)
    {
        float2 s = q * 0.5 + 0.5;
        float Inside2 = (all(s >= 0.0) && all(s <= 1.0)) ? 1.0 : 0.0;
        Gobo2 = Texture2DSampleBias(Gobo2Tex, Gobo2TexSampler, saturate(s), Blur).r * Inside2;
    }

    float Iris = 1.0 - smoothstep(IrisRadius - Soft, IrisRadius + Soft, r);

    float2 b = float2(cb * q.x - sb * q.y, sb * q.x + cb * q.y);
    float Top    = 1.0 - smoothstep((1.0 - BladeTop) - BladeSoft,    (1.0 - BladeTop) + BladeSoft,    b.y);
    float Bottom = smoothstep(-(1.0 - BladeBottom) - BladeSoft, -(1.0 - BladeBottom) + BladeSoft, b.y);
    float Right  = 1.0 - smoothstep((1.0 - BladeRight) - BladeSoft,  (1.0 - BladeRight) + BladeSoft,  b.x);
    float Left   = smoothstep(-(1.0 - BladeLeft) - BladeSoft,   -(1.0 - BladeLeft) + BladeSoft,   b.x);

    Mask = max(Mask, Gate * Gobo * Gobo2 * Iris * Top * Bottom * Right * Left);
}
return float3(Mask, Mask, Mask);
"""

BEAM_SCALARS = [
    ("GoboEnabled", 0.0), ("GoboRotation", 0.0), ("GoboSpinRpm", 0.0), ("GoboSpinPhase", 0.0), ("GoboSpinStart", 0.0),
    ("Gobo2Enabled", 0.0), ("IrisRadius", 1.0),
    ("BladeTop", 0.0), ("BladeRight", 0.0), ("BladeBottom", 0.0), ("BladeLeft", 0.0), ("BladeRotation", 0.0), ("BladeSoftness", 0.0),
    ("Frost", 0.0), ("PrismFacets", 0.0), ("PrismLinear", 0.0), ("PrismSpread", 0.0),
    ("PrismSpinRpm", 0.0), ("PrismSpinPhase", 0.0), ("PrismSpinStart", 0.0), ("ConeToBeam", 1.0),
]


def create_beam_shaping_material():
    material = load_or_create("M_StageBeamLightFunction", LIGHTING_DIR, unreal.Material, unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(material)
    material.set_editor_property("material_domain", unreal.MaterialDomain.MD_LIGHT_FUNCTION)

    inputs = ["UV", "Time", "GoboTex", "Gobo2Tex"] + [name for name, _ in BEAM_SCALARS]
    custom = add_custom(material, BEAM_SHAPING_HLSL, inputs, unreal.CustomMaterialOutputType.CMOT_FLOAT3, "Beam shaping")

    uv = mel.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -800, -300)
    mel.connect_material_expressions(uv, "", custom, "UV")
    time = mel.create_material_expression(material, unreal.MaterialExpressionTime, -800, -240)
    mel.connect_material_expressions(time, "", custom, "Time")
    add_texture_object(material, custom, "GoboTexture", "GoboTex", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE, -180)
    add_texture_object(material, custom, "Gobo2Texture", "Gobo2Tex", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE, -100)
    add_scalars(material, custom, BEAM_SCALARS, -20)

    mel.connect_material_property(custom, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.recompile_material(material)
    eal.save_loaded_asset(material)

    saved = eal.load_asset(LIGHTING_DIR + "/M_StageBeamLightFunction")
    names = [str(name) for name in mel.get_scalar_parameter_names(saved)]
    missing = [name for name, _ in BEAM_SCALARS if name not in names]
    if missing:
        raise RuntimeError("M_StageBeamLightFunction is missing parameters " + ", ".join(missing))
    log("Saved M_StageBeamLightFunction ({} scalars, 2 gobo textures)".format(len(names)))


# --- Lens / cell glow ---

EMITTER_HLSL = r"""
float3 Color = EmitterColor.rgb;
if (CellCount > 1.5)
{
    float Cells = CellCount;
    float u = saturate(UV.x) * Cells;
    float Cell = min(floor(u), Cells - 1.0);
    Color = Texture2DSampleLevel(CellTex, CellTexSampler, float2((Cell + 0.5) / Cells, 0.5), 0).rgb;
    float f = frac(u);
    Color *= smoothstep(0.0, 0.08, f) * smoothstep(1.0, 0.92, f);
}
return Color * Level * Gate * EmissiveStrength;
"""


def create_emitter_material():
    material = load_or_create("M_StageFixtureEmitter", LIGHTING_DIR, unreal.Material, unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(material)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)

    custom = add_custom(material, EMITTER_HLSL, ["UV", "EmitterColor", "CellTex", "CellCount", "Level", "Gate", "EmissiveStrength"],
                        unreal.CustomMaterialOutputType.CMOT_FLOAT3, "Lens and cells")
    uv = mel.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -800, -200)
    mel.connect_material_expressions(uv, "", custom, "UV")
    color = mel.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -800, -140)
    color.set_editor_property("parameter_name", "EmitterColor")
    color.set_editor_property("default_value", unreal.LinearColor(1.0, 1.0, 1.0, 1.0))
    mel.connect_material_expressions(color, "", custom, "EmitterColor")
    add_texture_object(material, custom, "CellTexture", "CellTex", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, -60)
    add_scalars(material, custom, [("CellCount", 1.0), ("Level", 1.0), ("Gate", 1.0), ("EmissiveStrength", 25.0)], 20)

    mel.connect_material_property(custom, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.recompile_material(material)
    eal.save_loaded_asset(material)

    saved = eal.load_asset(LIGHTING_DIR + "/M_StageFixtureEmitter")
    if saved.get_editor_property("shading_model") != unreal.MaterialShadingModel.MSM_UNLIT:
        raise RuntimeError("M_StageFixtureEmitter is not unlit")
    log("Saved M_StageFixtureEmitter")


# --- Gel library ---

def create_gel_table():
    path = LIGHTING_DIR + "/DT_StageGels"
    if eal.does_asset_exist(path):
        table = eal.load_asset(path)
    else:
        factory = unreal.DataTableFactory()
        factory.set_editor_property("struct", unreal.StageGelRow.static_struct())
        table = asset_tools.create_asset("DT_StageGels", LIGHTING_DIR, unreal.DataTable, factory)
    if not unreal.DataTableFunctionLibrary.fill_data_table_from_csv_file(table, GEL_CSV):
        raise RuntimeError("Could not fill DT_StageGels from " + GEL_CSV)
    eal.save_loaded_asset(table)

    with open(GEL_CSV, encoding="utf-8") as csv_file:
        expected = sum(1 for line in csv_file if line.strip()) - 1
    rows = unreal.DataTableFunctionLibrary.get_data_table_row_names(eal.load_asset(path))
    if len(rows) != expected:
        raise RuntimeError("DT_StageGels has {} rows, the CSV {}".format(len(rows), expected))
    log("Saved DT_StageGels ({} gels)".format(len(rows)))


# --- Base meshes ---

def create_base_mesh(name, size_cm):
    # The base is the actor's root, so it cannot be scaled at runtime without scaling the whole fixture: it is built at size.
    path = MESH_DIR + "/" + name
    if not eal.does_asset_exist(path):
        if eal.duplicate_asset(CUBE.split(".")[0], path) is None:
            raise RuntimeError("Could not create " + path)
    mesh = eal.load_asset(path)
    # Headless (-run=pythonscript) has no editor subsystems registered; the subsystem's mesh functions keep no state, so a
    # transient instance does the same work.
    subsystem = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem) or unreal.StaticMeshEditorSubsystem()
    settings = subsystem.get_lod_build_settings(mesh, 0)
    settings.set_editor_property("build_scale3d", unreal.Vector(size_cm[0] / 100.0, size_cm[1] / 100.0, size_cm[2] / 100.0))
    subsystem.set_lod_build_settings(mesh, 0, settings)
    subsystem.remove_collisions(mesh)
    subsystem.add_simple_collisions(mesh, unreal.ScriptCollisionShapeType.BOX)
    eal.save_loaded_asset(mesh)

    # Meshes build asynchronously, so the bounds may still be the cube's here; the saved build scale is the contract checked.
    scale = subsystem.get_lod_build_settings(eal.load_asset(path), 0).get_editor_property("build_scale3d")
    built = (scale.x * 100.0, scale.y * 100.0, scale.z * 100.0)
    if any(abs(built[i] - size_cm[i]) > 0.01 for i in range(3)):
        raise RuntimeError("{} builds at {} cm, expected {}".format(name, built, size_cm))
    return mesh


# --- Fixtures ---

GOBO_NAMES = [("Dots", 0.30), ("Star", 0.35), ("Bars", 0.45), ("Ring", 0.40), ("Fan", 0.42), ("Breakup", 0.38), ("Triangles", 0.25), ("Grid", 0.50)]


def gobo_slots(names):
    slots = []
    lookup = dict(GOBO_NAMES)
    for name in names:
        texture = eal.load_asset(GOBO_DIR + "/T_Gobo_" + name)
        if texture is None:
            raise RuntimeError("Missing gobo texture T_Gobo_" + name + " (run create_lighting_content.py first)")
        slot = unreal.StageGoboSlot()
        slot.set_editor_property("name", name)
        slot.set_editor_property("texture", texture)
        slot.set_editor_property("transmission", lookup[name])
        slots.append(slot)
    return slots


# Colour-wheel filters as linear transmission per channel.
COLOR_WHEEL = [
    ("Red", (1.0, 0.02, 0.02)), ("Orange", (1.0, 0.35, 0.0)), ("Yellow", (1.0, 0.85, 0.0)), ("Green", (0.05, 1.0, 0.1)),
    ("Light Blue", (0.25, 0.6, 1.0)), ("Congo Blue", (0.08, 0.02, 1.0)), ("Magenta", (1.0, 0.0, 0.6)), ("CTO", (1.0, 0.65, 0.4)),
    ("CTB", (0.65, 0.8, 1.0)),
]


def color_wheel():
    slots = []
    for name, (r, g, b) in COLOR_WHEEL:
        slot = unreal.StageColorWheelSlot()
        slot.set_editor_property("name", name)
        slot.set_editor_property("filter", unreal.LinearColor(r, g, b, 1.0))
        slots.append(slot)
    return slots


def prism(name, facets, linear, spread, transmission=0.85):
    value = unreal.StagePrism()
    value.set_editor_property("name", name)
    value.set_editor_property("facets", facets)
    value.set_editor_property("linear", linear)
    value.set_editor_property("spread", spread)
    value.set_editor_property("transmission", transmission)
    return value


def vec(values):
    return unreal.Vector(values[0], values[1], values[2])


def create_fixture(spec):
    name = spec["asset"]
    kind = spec["kind"]
    fixture = load_or_create(name, FIXTURE_DIR, unreal.LightingFixtureData, unreal.DataAssetFactory())
    fixture.set_editor_property("display_name", spec["name"])
    fixture.set_editor_property("description", spec["description"])
    fixture.set_editor_property("category_tag", make_tag(spec["category"]))
    fixture.set_editor_property("fixture_kind", kind)
    fixture.set_editor_property("color_system", spec.get("color", unreal.StageFixtureColorSystem.NONE))
    fixture.set_editor_property("color_temperature_k", spec.get("kelvin", 6500.0))
    fixture.set_editor_property("has_color_temperature_control", spec.get("ctc", False))
    fixture.set_editor_property("color_temperature_range_k", unreal.Vector2D(2700.0, 8000.0))
    fixture.set_editor_property("has_shutter", spec.get("shutter", True))
    fixture.set_editor_property("strobe_rate_hz", unreal.Vector2D(*spec.get("strobe_hz", (1.0, 20.0))))
    fixture.set_editor_property("emitters", [])
    fixture.set_editor_property("has_cto", spec.get("cto", False))
    fixture.set_editor_property("color_wheel", color_wheel() if spec.get("color_wheel") else [])
    fixture.set_editor_property("accepts_gel", spec.get("gel", False))

    fixture.set_editor_property("luminous_flux", spec["lumens"])
    fixture.set_editor_property("beam_angle_min", spec["beam"][0])
    fixture.set_editor_property("beam_angle_max", spec["beam"][1])
    fixture.set_editor_property("field_angle_ratio", spec.get("field_ratio", 1.5))
    fixture.set_editor_property("beam_range", spec.get("range", 4000.0))
    fixture.set_editor_property("gobo_wheel", gobo_slots(spec.get("gobos", [])))
    fixture.set_editor_property("gobo_spin", spec.get("gobo_spin", False))
    fixture.set_editor_property("static_gobo_wheel", gobo_slots(spec.get("static_gobos", [])))
    fixture.set_editor_property("prisms", spec.get("prisms", []))
    fixture.set_editor_property("has_frost", spec.get("frost", False))
    fixture.set_editor_property("has_iris", spec.get("iris", False))
    fixture.set_editor_property("beam_shaper", spec.get("shaper", unreal.StageBeamShaper.NONE))
    fixture.set_editor_property("cell_count", spec.get("cells", 1))
    fixture.set_editor_property("cell_control", spec.get("cell_control", unreal.StageCellControl.COLOR))

    moving = spec.get("pan_tilt", False)
    base, head = spec["base"], spec["head"]
    fixture.set_editor_property("mesh", create_base_mesh("SM_FixtureBase_" + name.replace("DA_Fixture_", ""), base))
    fixture.set_editor_property("has_pan_tilt", moving)
    fixture.set_editor_property("yoke_mesh", unreal.load_asset(CUBE) if moving else None)
    fixture.set_editor_property("head_mesh", unreal.load_asset(spec.get("head_mesh", CYLINDER)))
    # Base centred on the actor; the yoke sits on top of it, the head's centre at TiltPivot, the lens on the head's front.
    yoke_height = spec.get("yoke", (0, 0, 0))[2]
    fixture.set_editor_property("pan_pivot_offset", vec((0.0, 0.0, base[2] * 0.5)))
    fixture.set_editor_property("tilt_pivot_offset", vec((0.0, 0.0, (yoke_height * 0.5 if moving else 0.0) + spec.get("tilt_height", head[2] * 0.5 + 2.0))))
    fixture.set_editor_property("beam_origin_offset", vec((0.0, 0.0, head[2] * 0.5 + 0.6)))

    body = unreal.StageFixtureBody()
    body.set_editor_property("base_size_cm", vec(base))
    body.set_editor_property("yoke_size_cm", vec(spec.get("yoke", (0.0, 0.0, 0.0))))
    body.set_editor_property("head_size_cm", vec(head))
    body.set_editor_property("lens_diameter_cm", spec.get("lens", 0.0))
    body.set_editor_property("cell_strip_size_cm", unreal.Vector2D(*spec.get("strip", (0.0, 0.0))))
    fixture.set_editor_property("body", body)

    specs = unreal.StageEquipmentSpecs()
    specs.set_editor_property("manufacturer", "Generic")
    specs.set_editor_property("weight_kg", spec["weight"])
    specs.set_editor_property("power_draw_watts", spec["watts"])
    specs.set_editor_property("dimensions_cm", vec(spec["size"]))
    fixture.set_editor_property("specs", specs)

    fixture.generate_default_dmx_layout()
    if spec.get("dimmer_only_dmx"):
        # A conventional fixture is a lamp on a dimmer channel; its shutters, iris, gobo and gel are set by hand, so they
        # are attributes (inspector, cues) but not DMX channels.
        dimmer = unreal.StageFixtureDMXChannel()
        dimmer.set_editor_property("attribute", make_tag("StageCraft.Attribute.Dimmer"))
        dimmer.set_editor_property("channel", 1)
        dimmer.set_editor_property("b16_bit", False)
        fixture.set_editor_property("dmx_channels", [dimmer])
    fixture.set_editor_property("dmx_mode_name", "{} {}ch".format(spec["mode"], fixture.get_dmx_footprint()))
    eal.save_loaded_asset(fixture)

    saved = eal.load_asset(FIXTURE_DIR + "/" + name)
    attributes = saved.get_supported_attributes()
    if saved.get_dmx_footprint() < 1 or saved.get_dmx_footprint() > 512:
        raise RuntimeError("{}: DMX footprint {}".format(name, saved.get_dmx_footprint()))
    log("Saved {}: {} attributes, mode '{}'".format(name, len(attributes), saved.get_editor_property("dmx_mode_name")))


def fixture_specs():
    kinds = unreal.StageFixtureKind
    colors = unreal.StageFixtureColorSystem
    shapers = unreal.StageBeamShaper
    all_gobos = [name for name, _ in GOBO_NAMES]
    return [
        dict(asset="DA_Fixture_Spot470", name="Spot 470 (Moving Head)", kind=kinds.MOVING_HEAD_SPOT, category="StageCraft.Category.Lighting.MovingHead",
             mode="Extended", description="Moving head spot: CMY + variable CTO, 9-colour wheel, rotating (indexing, spinning) and static gobo wheels, 3-facet rotating prism, frost, iris, 5-50 degree zoom.",
             color=colors.CMY, kelvin=6800.0, cto=True, color_wheel=True, lumens=22000.0, beam=(5.0, 50.0), field_ratio=1.25,
             gobos=all_gobos[:7], gobo_spin=True, static_gobos=all_gobos, prisms=[prism("3-Facet", 3, False, 0.55)], frost=True, iris=True,
             pan_tilt=True, base=(43.0, 28.0, 16.0), yoke=(40.0, 14.0, 6.0), head=(26.0, 26.0, 40.0), lens=16.0, tilt_height=30.0,
             size=(43.0, 28.0, 70.0), weight=30.5, watts=750.0),
        dict(asset="DA_Fixture_WashRGBW", name="LED Wash 19x40W RGBW (Moving Head)", kind=kinds.MOVING_HEAD_WASH, category="StageCraft.Category.Lighting.MovingHead",
             mode="Standard", description="Moving head LED wash: RGBW with colour temperature correction, frost, 4-60 degree zoom.",
             color=colors.RGBW, ctc=True, lumens=18000.0, beam=(4.0, 60.0), field_ratio=1.8, frost=True,
             pan_tilt=True, base=(35.0, 26.0, 14.0), yoke=(34.0, 12.0, 6.0), head=(34.0, 34.0, 24.0), lens=30.0, tilt_height=24.0,
             size=(35.0, 35.0, 52.0), weight=21.0, watts=760.0),
        dict(asset="DA_Fixture_Beam", name="Beam 280 (Moving Head)", kind=kinds.MOVING_HEAD_BEAM, category="StageCraft.Category.Lighting.MovingHead",
             mode="Standard", description="Moving head beam: 3.5 degree beam, 9-colour wheel, static gobo wheel, 8-facet and 6-facet linear prisms, frost.",
             kelvin=7500.0, color_wheel=True, lumens=12000.0, beam=(3.5, 3.5), field_ratio=1.1, range=10000.0,
             static_gobos=all_gobos, prisms=[prism("8-Facet", 8, False, 0.9), prism("6-Facet Linear", 6, True, 0.9)], frost=True,
             pan_tilt=True, base=(34.0, 24.0, 14.0), yoke=(30.0, 12.0, 6.0), head=(20.0, 20.0, 30.0), lens=14.0, tilt_height=26.0,
             size=(34.0, 24.0, 50.0), weight=17.0, watts=470.0),
        dict(asset="DA_Fixture_Hybrid", name="Hybrid 440 (Moving Head)", kind=kinds.MOVING_HEAD_HYBRID, category="StageCraft.Category.Lighting.MovingHead",
             mode="Extended", description="Moving head hybrid (beam, spot and wash): CMY + CTO, colour wheel, rotating and static gobos, 8-facet and linear prisms, frost, iris, 2-42 degree zoom.",
             color=colors.CMY, kelvin=7000.0, cto=True, color_wheel=True, lumens=20000.0, beam=(2.0, 42.0), field_ratio=1.2,
             gobos=all_gobos[:7], gobo_spin=True, static_gobos=all_gobos, prisms=[prism("8-Facet", 8, False, 0.8), prism("4-Facet Linear", 4, True, 0.8)],
             frost=True, iris=True,
             pan_tilt=True, base=(40.0, 27.0, 16.0), yoke=(36.0, 13.0, 6.0), head=(24.0, 24.0, 36.0), lens=16.0, tilt_height=28.0,
             size=(40.0, 27.0, 62.0), weight=24.0, watts=600.0),
        dict(asset="DA_Fixture_ProfileMH", name="Profile 600 (Moving Head, Framing Shutters)", kind=kinds.MOVING_HEAD_SPOT, category="StageCraft.Category.Lighting.MovingHead",
             mode="Extended", description="Moving head profile: four framing shutters with rotation, CMY + CTO, rotating gobos, iris, frost, 7-55 degree zoom.",
             color=colors.CMY, kelvin=6500.0, cto=True, lumens=25000.0, beam=(7.0, 55.0), field_ratio=1.15,
             gobos=all_gobos[:7], gobo_spin=True, frost=True, iris=True, shaper=shapers.FRAMING_SHUTTERS,
             pan_tilt=True, base=(45.0, 30.0, 18.0), yoke=(42.0, 15.0, 6.0), head=(30.0, 30.0, 46.0), lens=18.0, tilt_height=34.0,
             size=(45.0, 30.0, 75.0), weight=38.0, watts=800.0),
        dict(asset="DA_Fixture_ParRGBWA", name="LED Par 12x15W RGBWA", kind=enum_value(kinds, "LED_PAR", "LE_D_PAR", "LEDPAR"), category="StageCraft.Category.Lighting.Par",
             mode="Standard", description="Static LED par: RGBWA colour mixing, 25 degree beam, strobe.",
             color=colors.RGBWA, lumens=4500.0, beam=(25.0, 25.0), field_ratio=1.6,
             base=(22.0, 6.0, 3.0), head=(22.0, 22.0, 14.0), lens=19.0, tilt_height=9.5,
             size=(25.0, 22.0, 28.0), weight=3.8, watts=180.0),
        dict(asset="DA_Fixture_Strobe", name="LED Strobe 1000", kind=kinds.STROBE, category="StageCraft.Category.Lighting.Strobe",
             mode="Basic", description="High-output white LED strobe, 1-25 flashes per second, 110 degree beam.",
             kelvin=6500.0, lumens=50000.0, beam=(110.0, 110.0), field_ratio=1.2, strobe_hz=(1.0, 25.0),
             base=(36.0, 8.0, 3.0), head=(40.0, 20.0, 12.0), head_mesh=CUBE, strip=(36.0, 16.0), tilt_height=7.5,
             size=(40.0, 20.0, 18.0), weight=12.0, watts=1000.0),
        dict(asset="DA_Fixture_Blinder4", name="LED Blinder 4-Cell 3200 K", kind=kinds.BLINDER, category="StageCraft.Category.Lighting.Strobe",
             mode="4-Cell", description="Warm-white audience blinder: four individually dimmed cells in a row, master dimmer and strobe, 50 degree beam.",
             kelvin=3200.0, lumens=24000.0, beam=(50.0, 50.0), field_ratio=1.4, cells=4, cell_control=unreal.StageCellControl.DIMMER,
             base=(60.0, 8.0, 3.0), head=(66.0, 20.0, 18.0), head_mesh=CUBE, strip=(62.0, 14.0), tilt_height=10.5,
             size=(66.0, 20.0, 24.0), weight=11.0, watts=400.0),
        dict(asset="DA_Fixture_PixelBar16", name="LED Pixel Bar 1 m 16x RGBW", kind=kinds.PIXEL_BAR, category="StageCraft.Category.Lighting.Pixel",
             mode="16-Pixel", description="1 m batten with 16 individually controlled RGBW pixels, master dimmer and strobe, 20 degree beam.",
             color=colors.RGBW, lumens=6000.0, beam=(20.0, 20.0), field_ratio=1.5, cells=16,
             base=(96.0, 6.0, 2.0), head=(100.0, 10.0, 10.0), head_mesh=CUBE, strip=(98.0, 4.0), tilt_height=6.0,
             size=(100.0, 12.0, 12.0), weight=5.5, watts=180.0),
        dict(asset="DA_Fixture_Ellipsoidal26", name="Ellipsoidal Profile 26 deg 750 W", kind=kinds.PROFILE, category="StageCraft.Category.Lighting.Conventional",
             mode="Dimmer", dimmer_only_dmx=True, description="Conventional ellipsoidal profile: fixed 26 degree lens, framing shutters with rotating barrel, gobo slot, iris, gel frame (3200 K tungsten, dimmer only).",
             kelvin=3200.0, gel=True, shutter=False, lumens=9500.0, beam=(26.0, 26.0), field_ratio=1.15,
             static_gobos=["Breakup", "Dots"], iris=True, shaper=shapers.FRAMING_SHUTTERS,
             base=(26.0, 6.0, 3.0), head=(26.0, 26.0, 58.0), lens=15.0, tilt_height=31.5,
             size=(30.0, 30.0, 66.0), weight=7.5, watts=750.0),
        dict(asset="DA_Fixture_Fresnel", name="Fresnel 1 kW", kind=kinds.FRESNEL, category="StageCraft.Category.Lighting.Conventional",
             mode="Dimmer", dimmer_only_dmx=True, description="Conventional fresnel: 15-60 degree spot/flood, four-leaf barndoors, gel frame (3200 K tungsten, dimmer only).",
             kelvin=3200.0, gel=True, shutter=False, lumens=12000.0, beam=(15.0, 60.0), field_ratio=1.9,
             shaper=shapers.BARNDOORS, base=(28.0, 6.0, 3.0), head=(28.0, 28.0, 30.0), lens=15.0, tilt_height=17.5,
             size=(30.0, 32.0, 40.0), weight=8.0, watts=1000.0),
    ]


create_beam_shaping_material()
create_emitter_material()
create_gel_table()
for fixture_spec in fixture_specs():
    create_fixture(fixture_spec)
log("Done.")
