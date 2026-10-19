# Marks the materials drawn on instanced static meshes as "Used with Instanced Static Meshes" (STATE.md #31).
# Spans (UStageSpanGeometryComponent) and their placement ghost draw every part as instances. Without the usage flag the
# editor patches the shader in memory on every launch ("missing usage flag InstancedStaticMeshes"), and a cooked build has
# no instanced shader for the material, so it draws the default material instead.
#
#   /Game/StageCraft/Spans/M_SpanMetal, M_SpanCoupler, M_SpanCable    span segment, joint and corner parts
#   /Game/StageCraft/Placement/M_PlacementGhost                       placement and span ghosts
#
# Headless (the project's editor must be closed):
#   UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script="<abs path>/fix_instanced_material_usage.py" -unattended -nullrhi
#
# Idempotent: a material that already has the flag is left untouched.

import unreal

MATERIALS = [
    "/Game/StageCraft/Spans/M_SpanMetal",
    "/Game/StageCraft/Spans/M_SpanCoupler",
    "/Game/StageCraft/Spans/M_SpanCable",
    "/Game/StageCraft/Placement/M_PlacementGhost",
]

eal = unreal.EditorAssetLibrary


def log(message):
    unreal.log("[StageCraftContent] " + message)


def main():
    for path in MATERIALS:
        material = eal.load_asset(path)
        if not isinstance(material, unreal.Material):
            raise RuntimeError("Missing or not a base material: " + path)
        if material.get_editor_property("used_with_instanced_static_meshes"):
            log(path + ": already used with instanced static meshes")
            continue
        material.set_editor_property("used_with_instanced_static_meshes", True)
        unreal.MaterialEditingLibrary.recompile_material(material)
        if not eal.save_loaded_asset(material):
            raise RuntimeError("Could not save " + path)
        log(path + ": usage flag set, saved")
    # Read back from the saved state, so a silent failure cannot pass.
    for path in MATERIALS:
        flag = eal.load_asset(path).get_editor_property("used_with_instanced_static_meshes")
        log(path + ": used_with_instanced_static_meshes = " + str(flag))
        if not flag:
            raise RuntimeError("Flag did not stick on " + path)


main()
