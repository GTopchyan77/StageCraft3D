# Creates the content for the AI Assistant (Docs/ADR/0016-ai-assistant.md):
#   /Game/StageCraft/UI/Panels/DA_Panel_Assistant   panel definition for UStageAIAssistantWidget (StageCraft.Panel.Assistant)
#
# Headless (no editor window needed; the project's editor must be closed):
#   UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script="<abs path>/create_assistant_content.py" -unattended -nullrhi
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


def create_panel():
    name = "DA_Panel_Assistant"
    tag_name = "StageCraft.Panel.Assistant"
    path = PANELS_DIR + "/" + name
    if eal.does_asset_exist(path):
        definition = eal.load_asset(path)
    else:
        definition = asset_tools.create_asset(name, PANELS_DIR, unreal.StagePanelDefinition, unreal.DataAssetFactory())
        if definition is None:
            raise RuntimeError("Could not create " + path)
    definition.set_editor_property("panel_tag", make_tag(tag_name))
    definition.set_editor_property("display_name", "AI Assistant")
    definition.set_editor_property("widget_class", unreal.StageAIAssistantWidget.static_class())
    eal.save_loaded_asset(definition)

    # Read back: a wrong tag would silently drop the panel from every layout.
    saved_tag = eal.load_asset(path).get_editor_property("panel_tag").export_text()
    if tag_name not in saved_tag:
        raise RuntimeError("{}: saved tag is {} instead of {}".format(name, saved_tag, tag_name))
    log("Saved {} (tag {})".format(name, saved_tag))


create_panel()
