# Creates the content for Sketch to Stage (Docs/ADR/0015-sketch-to-stage.md):
#   /Game/StageCraft/UI/Panels/DA_Panel_Sketch      panel definition for UStageSketchCanvasWidget (StageCraft.Panel.Sketch)
#   /Game/StageCraft/Decks/M_DeckSurface, M_DeckSkirt simple opaque materials for deck tops and skirts
#   /Game/StageCraft/Items/DA_Deck_Riser             stage deck / riser of any outline (UStageDeckData, 2 x 1 m panels, 20-200 cm)
#
# Headless (no editor window needed; the project's editor must be closed):
#   UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script="<abs path>/create_sketch_content.py" -unattended -nullrhi
#
# Idempotent: existing assets are updated in place (materials are rebuilt), so references stay valid.

import unreal

PANELS_DIR = "/Game/StageCraft/UI/Panels"
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


def create_panel():
    name = "DA_Panel_Sketch"
    tag_name = "StageCraft.Panel.Sketch"
    definition = load_or_create(name, PANELS_DIR, unreal.StagePanelDefinition, unreal.DataAssetFactory())
    definition.set_editor_property("panel_tag", make_tag(tag_name))
    definition.set_editor_property("display_name", "Sketch to Stage")
    definition.set_editor_property("widget_class", unreal.StageSketchCanvasWidget.static_class())
    eal.save_loaded_asset(definition)

    # Read back: a wrong tag would silently drop the panel from every layout.
    saved_tag = eal.load_asset(PANELS_DIR + "/" + name).get_editor_property("panel_tag").export_text()
    if tag_name not in saved_tag:
        raise RuntimeError("{}: saved tag is {} instead of {}".format(name, saved_tag, tag_name))
    log("Saved {} (tag {})".format(name, saved_tag))


def create_material(name, color, roughness):
    material = load_or_create(name, DECKS_DIR, unreal.Material, unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(material)
    base = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -300, 0)
    base.set_editor_property("constant", unreal.LinearColor(color[0], color[1], color[2], 1.0))
    rough = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -300, 200)
    rough.set_editor_property("r", roughness)
    mel.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.recompile_material(material)
    eal.save_loaded_asset(material)
    log("Saved " + material.get_path_name())
    return material


def create_deck(surface, skirt):
    name = "DA_Deck_Riser"
    item = load_or_create(name, ITEMS_DIR, unreal.StageDeckData, unreal.DataAssetFactory())
    item.set_editor_property("display_name", "Stage Deck / Riser (2 x 1 m panels)")
    item.set_editor_property("description", "Rental stage deck system, 2 x 1 m panels on legs from 20 cm to 2 m. Drawn to any outline in Sketch to Stage, or placed as one panel.")
    item.set_editor_property("category_tag", make_tag("StageCraft.Category.Stage.Deck"))
    item.set_editor_property("default_size", unreal.Vector2D(200.0, 100.0))
    item.set_editor_property("default_height", 60.0)
    item.set_editor_property("min_height", 20.0)
    item.set_editor_property("max_height", 200.0)
    item.set_editor_property("height_step", 10.0)
    item.set_editor_property("panel_size", unreal.Vector2D(200.0, 100.0))
    item.set_editor_property("surface_material", surface)
    item.set_editor_property("skirt_material", skirt)
    specs = unreal.StageEquipmentSpecs()
    specs.set_editor_property("manufacturer", "Generic")
    specs.set_editor_property("weight_kg", 28.0)
    specs.set_editor_property("power_draw_watts", 0.0)
    item.set_editor_property("specs", specs)
    eal.save_loaded_asset(item)

    saved = eal.load_asset(ITEMS_DIR + "/" + name)
    log("Saved {}: actor class {}, heights {}-{} cm, surface {}".format(name, saved.get_editor_property("actor_class"),
        saved.get_editor_property("min_height"), saved.get_editor_property("max_height"), saved.get_editor_property("surface_material")))


create_panel()
surface_material = create_material("M_DeckSurface", (0.11, 0.095, 0.085), 0.85)
skirt_material = create_material("M_DeckSkirt", (0.012, 0.012, 0.013), 0.95)
create_deck(surface_material, skirt_material)
log("Done.")
