# Creates the content for professional fixtures and the patch sheet (Docs/ADR/0007-lighting-patch-paths-and-pipeline-hardening.md):
#   /Game/StageCraft/Lighting/Gobos/T_Gobo_*              eight starter gobos, imported from Scripts/Content/Gobos (make_gobos.ps1)
#   /Game/StageCraft/Data/DA_MovingHead_Test              becomes a profile: CTC, gobo wheel, iris, framing shutters (DMX layout regenerated)
#   /Game/StageCraft/Data/DA_MovingHead_Wash_Test         gains CTC (DMX layout regenerated)
#   /Game/StageCraft/UI/Panels/DA_Panel_Patch             panel definition for UStagePatchPanel (StageCraft.Panel.Patch)
#
# Headless (no editor window needed; the project's editor must be closed):
#   UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script="<abs path>/create_lighting_content.py" -unattended -nullrhi
#
# Idempotent: textures are re-imported in place, data assets are updated, so references stay valid.
# M_StageBeamLightFunction is built by create_fixture_library_content.py (STATE.md #49), which needs these gobos first.

import os
import unreal

LIGHTING_DIR = "/Game/StageCraft/Lighting"
GOBO_DIR = LIGHTING_DIR + "/Gobos"
DATA_DIR = "/Game/StageCraft/Data"
PANELS_DIR = "/Game/StageCraft/UI/Panels"

# Wheel order and photometric transmission (open area) of each generated gobo.
GOBOS = [
    ("Dots", 0.30), ("Star", 0.35), ("Bars", 0.45), ("Ring", 0.40),
    ("Fan", 0.42), ("Breakup", 0.38), ("Triangles", 0.25), ("Grid", 0.50),
]

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


# --- Gobos ---

def import_gobos():
    source_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "Gobos")
    tasks = []
    for name, _ in GOBOS:
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", os.path.join(source_dir, "T_Gobo_" + name + ".png"))
        task.set_editor_property("destination_path", GOBO_DIR)
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("save", False)
        tasks.append(task)
    asset_tools.import_asset_tasks(tasks)

    textures = []
    for name, _ in GOBOS:
        path = GOBO_DIR + "/T_Gobo_" + name
        texture = eal.load_asset(path)
        if texture is None:
            raise RuntimeError("Gobo import failed: " + path)
        # A mask, not a colour: linear, single channel, clamped so the border (black) never repeats into the beam.
        texture.set_editor_property("srgb", False)
        texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_GRAYSCALE)
        texture.set_editor_property("address_x", unreal.TextureAddress.TA_CLAMP)
        texture.set_editor_property("address_y", unreal.TextureAddress.TA_CLAMP)
        texture.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_EFFECTS)
        eal.save_loaded_asset(texture)
        textures.append(texture)
        log("Saved " + path)
    return textures


# --- Fixtures ---

def make_gobo_wheel(textures):
    wheel = []
    for (name, transmission), texture in zip(GOBOS, textures):
        slot = unreal.StageGoboSlot()
        slot.set_editor_property("name", name)
        slot.set_editor_property("texture", texture)
        slot.set_editor_property("transmission", transmission)
        wheel.append(slot)
    return wheel


def update_fixture(name, mode_name, ctc, gobo_wheel, iris, shutters):
    path = DATA_DIR + "/" + name
    fixture = eal.load_asset(path)
    if fixture is None:
        raise RuntimeError("Missing fixture " + path)
    fixture.set_editor_property("has_color_temperature_control", ctc)
    fixture.set_editor_property("color_temperature_range_k", unreal.Vector2D(2700.0, 8000.0))
    fixture.set_editor_property("gobo_wheel", gobo_wheel)
    fixture.set_editor_property("has_iris", iris)
    fixture.set_editor_property("beam_shaper", unreal.StageBeamShaper.FRAMING_SHUTTERS if shutters else unreal.StageBeamShaper.NONE)
    fixture.generate_default_dmx_layout()
    fixture.set_editor_property("dmx_mode_name", "{} {}ch".format(mode_name, fixture.get_dmx_footprint()))
    eal.save_loaded_asset(fixture)
    log("Saved {} (mode {}, {} DMX channels)".format(path, fixture.get_editor_property("dmx_mode_name"), fixture.get_dmx_footprint()))


# --- Panel ---

def create_patch_panel():
    name = "DA_Panel_Patch"
    tag_name = "StageCraft.Panel.Patch"
    definition = load_or_create(name, PANELS_DIR, unreal.StagePanelDefinition, unreal.DataAssetFactory())
    definition.set_editor_property("panel_tag", make_tag(tag_name))
    definition.set_editor_property("display_name", "Patch")
    definition.set_editor_property("widget_class", unreal.StagePatchPanel.static_class())
    eal.save_loaded_asset(definition)

    # Read back: a wrong tag would silently drop the panel from every layout.
    saved_tag = eal.load_asset(PANELS_DIR + "/" + name).get_editor_property("panel_tag").export_text()
    if tag_name not in saved_tag:
        raise RuntimeError("{}: saved tag is {} instead of {}".format(name, saved_tag, tag_name))
    log("Saved {} (tag {})".format(name, saved_tag))


gobo_textures = import_gobos()

update_fixture("DA_MovingHead_Test", "Profile", True, make_gobo_wheel(gobo_textures), True, True)
update_fixture("DA_MovingHead_Wash_Test", "Wash", True, [], False, False)
create_patch_panel()
log("Done.")
