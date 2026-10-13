# Updates the span items for drawn paths (Docs/ADR/0007-lighting-patch-paths-and-pipeline-hardening.md §4):
#   /Game/StageCraft/Items/DA_Span_TrussF34    corner part: a round hub at every bend, so legs meet cleanly at any angle
#   /Game/StageCraft/Items/DA_Span_CableRun    may sag up to 15 % of a leg when hung between supports
# The railing keeps its posts at the bends (no corner parts: its joint parts are used there).
#
# Headless (the project's editor must be closed):
#   UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script="<abs path>/update_span_path_content.py" -unattended -nullrhi
#
# Idempotent: the properties are set, not appended.

import unreal

ITEMS_DIR = "/Game/StageCraft/Items"
SPANS_DIR = "/Game/StageCraft/Spans"
CYLINDER = "/Engine/BasicShapes/Cylinder.Cylinder"     # 100 cm tall along Z, 100 cm diameter, pivot at the centre

eal = unreal.EditorAssetLibrary


def log(message):
    unreal.log("[StageCraftContent] " + message)


def load(path):
    asset = eal.load_asset(path)
    if asset is None:
        raise RuntimeError("Missing " + path)
    return asset


def update_truss():
    truss = load(ITEMS_DIR + "/DA_Span_TrussF34")
    # A vertical hub is the same from every direction, so it fits a bend of any angle; 36 cm covers the 29 cm box's chord ends.
    hub = unreal.StageSpanPart()
    hub.set_editor_property("mesh", load(CYLINDER))
    hub.set_editor_property("material", load(SPANS_DIR + "/M_SpanCoupler"))
    hub.set_editor_property("transform", unreal.Transform(location=unreal.Vector(0.0, 0.0, 14.5), rotation=unreal.Rotator(), scale=unreal.Vector(0.36, 0.36, 0.31)))
    truss.set_editor_property("corner_parts", [hub])
    truss.set_editor_property("description", "Box truss drawn as a path of straight legs, in whole 2 m sections, with a hub at every bend. Weight is per section.")
    eal.save_loaded_asset(truss)
    log("Saved DA_Span_TrussF34 ({} corner parts)".format(len(load(ITEMS_DIR + "/DA_Span_TrussF34").get_editor_property("corner_parts"))))


def update_cable():
    cable = load(ITEMS_DIR + "/DA_Span_CableRun")
    cable.set_editor_property("max_sag_percent", 15.0)
    cable.set_editor_property("description", "Cable drawn as a path; sections stretch to each leg's exact length, and a hung run can sag. Billed per metre.")
    eal.save_loaded_asset(cable)
    log("Saved DA_Span_CableRun (max sag {} %)".format(load(ITEMS_DIR + "/DA_Span_CableRun").get_editor_property("max_sag_percent")))


update_truss()
update_cable()
log("Done.")
