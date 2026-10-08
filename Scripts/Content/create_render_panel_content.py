# Creates the content asset for the Render panel (Docs/ADR/0004-selection-scenes-and-rendering.md §5.4):
#   /Game/StageCraft/UI/Panels/DA_Panel_Render   workspace panel definition for UStageRenderPanel (tag StageCraft.Panel.Render)
#
# Headless (no editor window needed; the project's editor must be closed):
#   UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script="<abs path>/create_render_panel_content.py" -unattended -nullrhi
#
# Idempotent: an existing asset is updated in place, so the script is the source of truth for it.

import unreal

PANELS_DIR = "/Game/StageCraft/UI/Panels"

eal = unreal.EditorAssetLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()


def log(message):
    unreal.log("[StageCraftContent] " + message)


def create_render_panel_definition():
    name = "DA_Panel_Render"
    path = PANELS_DIR + "/" + name
    # Updated in place when it exists, so references to it stay valid and re-running is harmless.
    if eal.does_asset_exist(path):
        definition = eal.load_asset(path)
    else:
        definition = asset_tools.create_asset(name, PANELS_DIR, unreal.StagePanelDefinition, unreal.DataAssetFactory())
    if definition is None:
        raise RuntimeError("Could not create or load " + path)

    tag = unreal.GameplayTag()
    tag.import_text('(TagName="StageCraft.Panel.Render")')
    definition.set_editor_property("panel_tag", tag)
    definition.set_editor_property("display_name", "Render")
    definition.set_editor_property("widget_class", unreal.StageRenderPanel.static_class())
    eal.save_loaded_asset(definition)

    # Read back what was saved: a wrong tag would silently drop the panel from every layout.
    saved = eal.load_asset(path)
    saved_tag = saved.get_editor_property("panel_tag").export_text()
    if "StageCraft.Panel.Render" not in saved_tag:
        raise RuntimeError("Saved tag is {} instead of StageCraft.Panel.Render".format(saved_tag))
    log("Saved {} (tag {}, widget {})".format(path, saved_tag, saved.get_editor_property("widget_class")))


create_render_panel_definition()
log("Done.")
