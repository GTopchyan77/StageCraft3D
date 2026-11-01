# Creates the content for the Send to 3D App panel (Docs/ADR/0022-dcc-bridge.md §7):
#   /Game/StageCraft/UI/Panels/DA_Panel_SendTo   panel definition for UStageSendToWidget (StageCraft.Panel.SendTo)
#
# Headless (no editor window needed; the project's editor must be closed):
#   UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script="<abs path>/create_send_to_content.py" -unattended -nullrhi
#
# Idempotent: an existing asset is updated in place, so references stay valid.

import unreal

PANELS_DIR = "/Game/StageCraft/UI/Panels"

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


def create_panel():
    name = "DA_Panel_SendTo"
    tag_name = "StageCraft.Panel.SendTo"
    definition = load_or_create(name, PANELS_DIR, unreal.StagePanelDefinition, unreal.DataAssetFactory())
    definition.set_editor_property("panel_tag", make_tag(tag_name))
    definition.set_editor_property("display_name", "Send to 3D App")
    definition.set_editor_property("widget_class", unreal.StageSendToWidget.static_class())
    eal.save_loaded_asset(definition)

    # Read back: a wrong tag would silently drop the panel from every layout.
    saved_tag = eal.load_asset(PANELS_DIR + "/" + name).get_editor_property("panel_tag").export_text()
    if tag_name not in saved_tag:
        raise RuntimeError("{}: saved tag is {} instead of {}".format(name, saved_tag, tag_name))
    log("Saved {} (tag {})".format(name, saved_tag))


create_panel()
log("Send to 3D App content done.")
