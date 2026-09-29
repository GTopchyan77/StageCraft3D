"""
StageCraft 3D: generate the default Enhanced Input assets and framework Blueprints.

Idempotent: existing assets are reused and re-configured to the defaults below, never duplicated.
Run headless (the editor for this project must be closed):

    UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script="<Project>/Content/Python/stagecraft_setup_content.py" -unattended -nop4 -nosplash

Or from the editor: Tools > Execute Python Script.

Creates:
    /Game/StageCraft/Input/IA_Place, IA_Delete, IA_ToggleGizmoMode, IMC_StageEditor
    /Game/StageCraft/Blueprints/BP_ModularPlayerController  (parent AModularPlayerController, input slots assigned)
    /Game/StageCraft/Blueprints/BP_StageCraftGameMode       (parent AStageCraftGameModeBase, uses the controller BP)

The mappings mirror AModularPlayerController::BuildDefaultInputMapping(), so behavior with and without
these assets is identical; designers rebind keys by editing IMC_StageEditor.
"""

import unreal

INPUT_DIR = "/Game/StageCraft/Input"
BLUEPRINT_DIR = "/Game/StageCraft/Blueprints"

# ETriggerEvent bit flags (EnhancedInput/Public/InputTriggers.h).
TRIGGER_TRIGGERED = 1 << 0
TRIGGER_STARTED = 1 << 1
TRIGGER_ONGOING = 1 << 2

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
eal = unreal.EditorAssetLibrary


def log(msg):
    unreal.log("[StageCraftSetup] " + msg)


def get_or_create(asset_dir, name, asset_class, factory):
    path = f"{asset_dir}/{name}"
    if eal.does_asset_exist(path):
        asset = eal.load_asset(path)
        if not isinstance(asset, asset_class):
            raise RuntimeError(f"{path} exists but is a {type(asset).__name__}, expected {asset_class.__name__}")
        log(f"Reusing {path}")
        return asset
    asset = asset_tools.create_asset(name, asset_dir, asset_class, factory)
    if asset is None:
        raise RuntimeError(f"Failed to create {path}")
    log(f"Created {path}")
    return asset


def get_or_create_blueprint(name, parent_class):
    path = f"{BLUEPRINT_DIR}/{name}"
    if eal.does_asset_exist(path):
        bp = eal.load_asset(path)
        log(f"Reusing {path}")
        return bp
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", parent_class)
    bp = asset_tools.create_asset(name, BLUEPRINT_DIR, unreal.Blueprint, factory)
    if bp is None:
        raise RuntimeError(f"Failed to create {path}")
    log(f"Created {path}")
    return bp


def blueprint_cdo(bp):
    # Compile first so the generated class exists; CDO edits made after the final compile are what gets saved.
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    return unreal.get_default_object(unreal.BlueprintEditorLibrary.generated_class(bp))


def make_action(name, consume_legacy_keys=False):
    action = get_or_create(INPUT_DIR, name, unreal.InputAction, unreal.InputAction_Factory())
    action.set_editor_property("value_type", unreal.InputActionValueType.BOOLEAN)
    action.set_editor_property("consumes_action_and_axis_mappings", consume_legacy_keys)
    if consume_legacy_keys:
        action.set_editor_property("trigger_events_that_consume_legacy_keys",
                                   TRIGGER_STARTED | TRIGGER_ONGOING | TRIGGER_TRIGGERED)
    return action


def main():
    place = make_action("IA_Place")
    delete = make_action("IA_Delete")
    # Space is also ADefaultPawn's legacy "fly up" key; consuming it stops the camera rising on toggle.
    toggle = make_action("IA_ToggleGizmoMode", consume_legacy_keys=True)

    imc = get_or_create(INPUT_DIR, "IMC_StageEditor", unreal.InputMappingContext, unreal.InputMappingContext_Factory())
    imc.unmap_all()
    for action, key_name in ((place, "LeftMouseButton"), (delete, "RightMouseButton"), (toggle, "SpaceBar")):
        key = unreal.Key()
        key.set_editor_property("key_name", key_name)
        imc.map_key(action, key)

    controller_bp = get_or_create_blueprint("BP_ModularPlayerController", unreal.ModularPlayerController)
    controller_cdo = blueprint_cdo(controller_bp)
    controller_cdo.set_editor_property("editor_mapping_context", imc)
    controller_cdo.set_editor_property("place_action", place)
    controller_cdo.set_editor_property("delete_action", delete)
    controller_cdo.set_editor_property("toggle_gizmo_mode_action", toggle)

    game_mode_bp = get_or_create_blueprint("BP_StageCraftGameMode", unreal.StageCraftGameModeBase)
    game_mode_cdo = blueprint_cdo(game_mode_bp)
    game_mode_cdo.set_editor_property("player_controller_class",
                                      unreal.BlueprintEditorLibrary.generated_class(controller_bp))

    for asset in (place, delete, toggle, imc, controller_bp, game_mode_bp):
        path = asset.get_path_name().split(".")[0]
        if not eal.save_asset(path, only_if_is_dirty=False):
            raise RuntimeError(f"Failed to save {path}")
        log(f"Saved {path}")

    log("Done.")


main()
