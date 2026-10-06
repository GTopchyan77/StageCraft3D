# Creates the content assets for placement mode and editor audio (Docs/ADR/0002-placement-and-audio.md):
#   /Game/StageCraft/Placement/M_PlacementGhost   translucent unlit ghost, "GhostColor" vector parameter
#   /Game/StageCraft/Placement/M_PlacementMarker  deferred decal ring, "MarkerColor" vector parameter
#   /Game/StageCraft/Audio/Feedback/SFX_*         feedback sounds imported from Scripts/Content/make_feedback_wavs.ps1 output
#   /Game/StageCraft/UI/Panels/DA_Panel_Library   workspace panel definition for UStageItemLibraryPanel
#
# Headless (no editor window needed; the project's editor must be closed):
#   powershell -File Scripts/Content/make_feedback_wavs.ps1
#   UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script="<abs path>/create_placement_audio_content.py" -unattended -nullrhi
#
# Idempotent: existing assets are replaced, so the script is the source of truth for these assets.

import os
import unreal

PLACEMENT_DIR = "/Game/StageCraft/Placement"
AUDIO_DIR = "/Game/StageCraft/Audio/Feedback"
PANELS_DIR = "/Game/StageCraft/UI/Panels"
WAV_DIR = os.path.normpath(os.path.join(unreal.Paths.project_saved_dir(), "ContentSource", "Feedback"))

mel = unreal.MaterialEditingLibrary
eal = unreal.EditorAssetLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()


def log(message):
    unreal.log("[StageCraftContent] " + message)


def fresh_material(name):
    path = PLACEMENT_DIR + "/" + name
    if eal.does_asset_exist(path):
        eal.delete_asset(path)
    material = asset_tools.create_asset(name, PLACEMENT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        raise RuntimeError("Could not create " + path)
    return material


def node(material, expression_class, x, y, **properties):
    expression = mel.create_material_expression(material, expression_class, x, y)
    for key, value in properties.items():
        expression.set_editor_property(key, value)
    return expression


def link(from_expression, to_expression, to_input, from_output=""):
    if not mel.connect_material_expressions(from_expression, from_output, to_expression, to_input):
        raise RuntimeError("Could not connect {} -> {}.{}".format(from_expression.get_name(), to_expression.get_name(), to_input))


def output(material, from_expression, material_property, from_output=""):
    if not mel.connect_material_property(from_expression, from_output, material_property):
        raise RuntimeError("Could not connect {} to {}".format(from_expression.get_name(), material_property))


def finish(material):
    mel.recompile_material(material)
    eal.save_loaded_asset(material)
    log("Saved " + material.get_path_name())


def create_ghost_material():
    material = fresh_material("M_PlacementGhost")
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("two_sided", True)

    color = node(material, unreal.MaterialExpressionVectorParameter, -600, -100,
                 parameter_name="GhostColor", default_value=unreal.LinearColor(0.1, 0.75, 1.0, 1.0))
    brightness = node(material, unreal.MaterialExpressionMultiply, -300, -100, const_b=1.5)
    link(color, brightness, "A")
    output(material, brightness, unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    # Edges (fresnel) more opaque than faces, so the silhouette reads against any background.
    fresnel = node(material, unreal.MaterialExpressionFresnel, -600, 150, exponent=3.0)
    opacity = node(material, unreal.MaterialExpressionLinearInterpolate, -300, 150, const_a=0.25, const_b=0.7)
    link(fresnel, opacity, "Alpha")
    output(material, opacity, unreal.MaterialProperty.MP_OPACITY)
    finish(material)


def create_marker_material():
    material = fresh_material("M_PlacementMarker")
    material.set_editor_property("material_domain", unreal.MaterialDomain.MD_DEFERRED_DECAL)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)

    color = node(material, unreal.MaterialExpressionVectorParameter, -700, -200,
                 parameter_name="MarkerColor", default_value=unreal.LinearColor(0.1, 0.75, 1.0, 1.0))
    output(material, color, unreal.MaterialProperty.MP_BASE_COLOR)
    emissive = node(material, unreal.MaterialExpressionMultiply, -400, -200, const_b=3.0)
    link(color, emissive, "A")
    output(material, emissive, unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    # d = 0 at the centre of the footprint, 1 at its edge.
    uv = node(material, unreal.MaterialExpressionTextureCoordinate, -1300, 150)
    centred = node(material, unreal.MaterialExpressionSubtract, -1100, 150, const_b=0.5)
    link(uv, centred, "A")
    length = node(material, unreal.MaterialExpressionLength, -950, 150)
    link(centred, length, "")
    distance = node(material, unreal.MaterialExpressionMultiply, -800, 150, const_b=2.0)
    link(length, distance, "A")

    # Ring: bright band just inside the edge.
    ring_offset = node(material, unreal.MaterialExpressionSubtract, -650, 100, const_b=0.85)
    link(distance, ring_offset, "A")
    ring_abs = node(material, unreal.MaterialExpressionAbs, -500, 100)
    link(ring_offset, ring_abs, "")
    ring_width = node(material, unreal.MaterialExpressionDivide, -350, 100, const_b=0.1)
    link(ring_abs, ring_width, "A")
    ring = node(material, unreal.MaterialExpressionOneMinus, -200, 100)
    link(ring_width, ring, "")

    # Fill: faint disc inside the ring.
    fill_edge = node(material, unreal.MaterialExpressionSubtract, -650, 300, const_b=0.97)
    link(distance, fill_edge, "A")
    fill_sharp = node(material, unreal.MaterialExpressionMultiply, -500, 300, const_b=30.0)
    link(fill_edge, fill_sharp, "A")
    fill_clamped = node(material, unreal.MaterialExpressionSaturate, -350, 300)
    link(fill_sharp, fill_clamped, "")
    fill = node(material, unreal.MaterialExpressionOneMinus, -200, 300)
    link(fill_clamped, fill, "")
    fill_faint = node(material, unreal.MaterialExpressionMultiply, -50, 300, const_b=0.15)
    link(fill, fill_faint, "A")

    ring_clamped = node(material, unreal.MaterialExpressionSaturate, -50, 100)
    link(ring, ring_clamped, "")
    combined = node(material, unreal.MaterialExpressionAdd, 100, 200)
    link(ring_clamped, combined, "A")
    link(fill_faint, combined, "B")
    opacity = node(material, unreal.MaterialExpressionSaturate, 250, 200)
    link(combined, opacity, "")
    output(material, opacity, unreal.MaterialProperty.MP_OPACITY)
    finish(material)


def import_sounds():
    tasks = []
    for name in ("SFX_Select", "SFX_Place", "SFX_Snap", "SFX_Error"):
        source = os.path.join(WAV_DIR, name + ".wav")
        if not os.path.isfile(source):
            raise RuntimeError("Missing " + source + " (run Scripts/Content/make_feedback_wavs.ps1 first)")
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", source)
        task.set_editor_property("destination_path", AUDIO_DIR)
        task.set_editor_property("destination_name", name)
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("save", True)
        tasks.append(task)

    asset_tools.import_asset_tasks(tasks)
    for task in tasks:
        imported = task.get_editor_property("imported_object_paths")
        if not imported:
            raise RuntimeError("Import failed: " + task.get_editor_property("filename"))
        log("Imported " + ", ".join(imported))


def create_library_panel_definition():
    name = "DA_Panel_Library"
    path = PANELS_DIR + "/" + name
    if eal.does_asset_exist(path):
        eal.delete_asset(path)
    definition = asset_tools.create_asset(name, PANELS_DIR, unreal.StagePanelDefinition, unreal.DataAssetFactory())
    if definition is None:
        raise RuntimeError("Could not create " + path)

    tag = unreal.GameplayTag()
    tag.import_text('(TagName="StageCraft.Panel.Library")')
    definition.set_editor_property("panel_tag", tag)
    definition.set_editor_property("display_name", "Library")
    definition.set_editor_property("widget_class", unreal.StageItemLibraryPanel.static_class())
    eal.save_loaded_asset(definition)
    log("Saved {} (tag {})".format(path, definition.get_editor_property("panel_tag")))


create_ghost_material()
create_marker_material()
import_sounds()
create_library_panel_definition()
log("Done.")
