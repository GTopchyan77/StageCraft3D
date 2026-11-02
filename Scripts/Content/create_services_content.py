# Creates the content for the Services & Keys panel (Docs/ADR/0023-ai-tool-providers-and-credential-vault.md §5):
#   /Game/StageCraft/UI/Panels/DA_Panel_Services   panel definition for UStageServicesWidget (StageCraft.Panel.Services)
#
# Headless (no editor window needed; the project's editor must be closed):
#   UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script="<abs path>/create_services_content.py" -unattended -nullrhi
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
    name = "DA_Panel_Services"
    tag_name = "StageCraft.Panel.Services"
    definition = load_or_create(name, PANELS_DIR, unreal.StagePanelDefinition, unreal.DataAssetFactory())
    definition.set_editor_property("panel_tag", make_tag(tag_name))
    definition.set_editor_property("display_name", "Services & Keys")
    definition.set_editor_property("widget_class", unreal.StageServicesWidget.static_class())
    eal.save_loaded_asset(definition)

    # Read back: a wrong tag would silently drop the panel from every layout.
    saved_tag = eal.load_asset(PANELS_DIR + "/" + name).get_editor_property("panel_tag").export_text()
    if tag_name not in saved_tag:
        raise RuntimeError("{}: saved tag is {} instead of {}".format(name, saved_tag, tag_name))
    log("Saved {} (tag {})".format(name, saved_tag))


create_panel()
log("Services & Keys content done.")
