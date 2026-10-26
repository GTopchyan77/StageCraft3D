# Creates the catalog items for the assistant's procedural stage shapes (Docs/ADR/0018 §5):
#   /Game/StageCraft/Shapes/M_Handrail, M_TrussArc   simple opaque materials (brushed steel rails, aluminium truss)
#   /Game/StageCraft/Items/DA_Stairs_Stage            stage stairs, 20-200 cm rise, 60-400 cm wide, optional handrails (UStageStairsData)
#   /Game/StageCraft/Items/DA_Truss_Arc_F34           curved F34 box truss, 1-20 m radius, 15-360 degrees, 2 m rental sections (UStageArcTrussData)
# Round and curved risers need no item of their own: they are outlines of DA_Deck_Riser (create_sketch_content.py).
#
# Headless (no editor window needed; the project's editor must be closed):
#   UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script="<abs path>/create_procedural_shapes_content.py" -unattended -nullrhi
#
# Idempotent: existing assets are updated in place (materials are rebuilt), so references stay valid.

import unreal

SHAPES_DIR = "/Game/StageCraft/Shapes"
DECKS_DIR = "/Game/StageCraft/Decks"
ITEMS_DIR = "/Game/StageCraft/Items"

eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary
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


def create_material(name, color, metallic, roughness):
    material = load_or_create(name, SHAPES_DIR, unreal.Material, unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(material)
    base = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -300, 0)
    base.set_editor_property("constant", unreal.LinearColor(color[0], color[1], color[2], 1.0))
    metal = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -300, 150)
    metal.set_editor_property("r", metallic)
    rough = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -300, 300)
    rough.set_editor_property("r", roughness)
    mel.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(metal, "", unreal.MaterialProperty.MP_METALLIC)
    mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.recompile_material(material)
    eal.save_loaded_asset(material)
    log("Saved " + material.get_path_name())
    return material


def make_specs(weight_kg):
    specs = unreal.StageEquipmentSpecs()
    specs.set_editor_property("manufacturer", "Generic")
    specs.set_editor_property("weight_kg", weight_kg)
    specs.set_editor_property("power_draw_watts", 0.0)
    return specs


def create_stairs(step_material, handrail_material):
    name = "DA_Stairs_Stage"
    item = load_or_create(name, ITEMS_DIR, unreal.StageStairsData, unreal.DataAssetFactory())
    item.set_editor_property("display_name", "Stage Stairs")
    item.set_editor_property("description", "Rental stage stairs up to a deck: equal steps of at most 20 cm on 30 cm treads, 20 cm to 2 m rise, 60 cm to 4 m wide, handrails on both sides.")
    item.set_editor_property("category_tag", make_tag("StageCraft.Category.Stage.Riser"))
    item.set_editor_property("default_rise", 60.0)
    item.set_editor_property("min_rise", 20.0)
    item.set_editor_property("max_rise", 200.0)
    item.set_editor_property("default_width", 120.0)
    item.set_editor_property("min_width", 60.0)
    item.set_editor_property("max_width", 400.0)
    item.set_editor_property("tread_depth", 30.0)
    item.set_editor_property("max_step_rise", 20.0)
    item.set_editor_property("size_step", 10.0)
    item.set_editor_property("default_handrails", True)
    item.set_editor_property("step_material", step_material)
    item.set_editor_property("handrail_material", handrail_material)
    item.set_editor_property("specs", make_specs(35.0))
    eal.save_loaded_asset(item)

    saved = eal.load_asset(ITEMS_DIR + "/" + name)
    log("Saved {}: actor class {}, rise {}-{} cm, width {}-{} cm".format(name, saved.get_editor_property("actor_class"),
        saved.get_editor_property("min_rise"), saved.get_editor_property("max_rise"), saved.get_editor_property("min_width"),
        saved.get_editor_property("max_width")))


def create_arc_truss(truss_material):
    name = "DA_Truss_Arc_F34"
    item = load_or_create(name, ITEMS_DIR, unreal.StageArcTrussData, unreal.DataAssetFactory())
    item.set_editor_property("display_name", "F34 Curved Truss (2 m sections)")
    item.set_editor_property("description", "Square 29 cm aluminium box truss bent to any radius from 1 to 20 m, as an arc or a full circle. Counted in 2 m curved rental sections.")
    item.set_editor_property("category_tag", make_tag("StageCraft.Category.Truss.Curved"))
    item.set_editor_property("default_radius", 400.0)
    item.set_editor_property("min_radius", 100.0)
    item.set_editor_property("max_radius", 2000.0)
    item.set_editor_property("default_angle", 90.0)
    item.set_editor_property("min_angle", 15.0)
    item.set_editor_property("max_angle", 360.0)
    item.set_editor_property("section_length", 200.0)
    item.set_editor_property("profile_width", 29.0)
    item.set_editor_property("chord_diameter", 5.0)
    item.set_editor_property("brace_diameter", 2.0)
    item.set_editor_property("radius_step", 25.0)
    item.set_editor_property("truss_material", truss_material)
    # Per 2 m curved section, as straight F34 is specified per segment.
    item.set_editor_property("specs", make_specs(13.0))
    eal.save_loaded_asset(item)

    saved = eal.load_asset(ITEMS_DIR + "/" + name)
    log("Saved {}: actor class {}, radius {}-{} cm, angle {}-{} deg".format(name, saved.get_editor_property("actor_class"),
        saved.get_editor_property("min_radius"), saved.get_editor_property("max_radius"), saved.get_editor_property("min_angle"),
        saved.get_editor_property("max_angle")))


deck_surface_path = DECKS_DIR + "/M_DeckSurface"
if not eal.does_asset_exist(deck_surface_path):
    raise RuntimeError(deck_surface_path + " is missing: run create_sketch_content.py first")
deck_surface = eal.load_asset(deck_surface_path)
handrail = create_material("M_Handrail", (0.55, 0.56, 0.58), 1.0, 0.35)
truss = create_material("M_TrussArc", (0.80, 0.81, 0.83), 1.0, 0.3)
create_stairs(deck_surface, handrail)
create_arc_truss(truss)
log("Done.")
