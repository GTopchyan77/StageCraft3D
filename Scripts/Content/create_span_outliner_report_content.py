# Creates the content for spans, the outliner and the bill of materials (Docs/ADR/0005-spans-outliner-and-bill-of-materials.md):
#   /Game/StageCraft/UI/Panels/DA_Panel_Outliner          panel definition for UStageOutlinerPanel        (StageCraft.Panel.Outliner)
#   /Game/StageCraft/UI/Panels/DA_Panel_BillOfMaterials   panel definition for UStageBillOfMaterialsPanel (StageCraft.Panel.BillOfMaterials)
#   /Game/StageCraft/Spans/M_SpanMetal, M_SpanCoupler, M_SpanCable   simple opaque materials for the span parts
#   /Game/StageCraft/Items/DA_Span_TrussF34    box truss run, whole 2 m sections (chords + lacing + couplers from engine primitives)
#   /Game/StageCraft/Items/DA_Span_Railing     handrail, 1 m sections stretched to length, posts at every joint
#   /Game/StageCraft/Items/DA_Span_CableRun    floor cable, 1 m sections stretched to length
#
# Headless (no editor window needed; the project's editor must be closed):
#   UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script="<abs path>/create_span_outliner_report_content.py" -unattended -nullrhi
#
# Idempotent: existing assets are updated in place (materials are rebuilt), so references stay valid and re-running is harmless.

import math
import unreal

PANELS_DIR = "/Game/StageCraft/UI/Panels"
SPANS_DIR = "/Game/StageCraft/Spans"
ITEMS_DIR = "/Game/StageCraft/Items"
CYLINDER = "/Engine/BasicShapes/Cylinder.Cylinder"     # 100 cm tall along Z, 100 cm diameter, pivot at the centre
CUBE = "/Engine/BasicShapes/Cube.Cube"                 # 100 cm, pivot at the centre

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


# --- Panels ---

def create_panel(name, tag_name, display_name, widget_class):
    definition = load_or_create(name, PANELS_DIR, unreal.StagePanelDefinition, unreal.DataAssetFactory())
    definition.set_editor_property("panel_tag", make_tag(tag_name))
    definition.set_editor_property("display_name", display_name)
    definition.set_editor_property("widget_class", widget_class)
    eal.save_loaded_asset(definition)

    # Read back: a wrong tag would silently drop the panel from every layout.
    saved_tag = eal.load_asset(PANELS_DIR + "/" + name).get_editor_property("panel_tag").export_text()
    if tag_name not in saved_tag:
        raise RuntimeError("{}: saved tag is {} instead of {}".format(name, saved_tag, tag_name))
    log("Saved {} (tag {})".format(name, saved_tag))


# --- Materials ---

def create_material(name, color, metallic, roughness):
    material = load_or_create(name, SPANS_DIR, unreal.Material, unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(material)

    base = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -300, 0)
    base.set_editor_property("constant", unreal.LinearColor(color[0], color[1], color[2], 1.0))
    metal = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -300, 150)
    metal.set_editor_property("r", metallic)
    rough = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -300, 250)
    rough.set_editor_property("r", roughness)
    mel.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(metal, "", unreal.MaterialProperty.MP_METALLIC)
    mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.recompile_material(material)
    eal.save_loaded_asset(material)
    log("Saved " + material.get_path_name())
    return material


# --- Span parts ---

def part(mesh, material, location, rotation=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0)):
    """rotation = (pitch, yaw, roll) in degrees."""
    span_part = unreal.StageSpanPart()
    span_part.set_editor_property("mesh", mesh)
    span_part.set_editor_property("material", material)
    span_part.set_editor_property("transform", unreal.Transform(
        location=unreal.Vector(*location),
        rotation=unreal.Rotator(roll=rotation[2], pitch=rotation[0], yaw=rotation[1]),
        scale=unreal.Vector(*scale)))
    return span_part


def tube_along_x(mesh, material, centre, length, diameter):
    # Pitch 90 turns the cylinder's height axis (Z) onto the run's X axis.
    return part(mesh, material, centre, (90.0, 0.0, 0.0), (diameter / 100.0, diameter / 100.0, length / 100.0))


def make_specs(manufacturer, weight_kg, power_w):
    specs = unreal.StageEquipmentSpecs()
    specs.set_editor_property("manufacturer", manufacturer)
    specs.set_editor_property("weight_kg", weight_kg)
    specs.set_editor_property("power_draw_watts", power_w)
    return specs


def save_span(name, display_name, description, category, item_type, fit, segment_length, max_segments, segment_parts, joint_parts, specs, grid):
    item = load_or_create(name, ITEMS_DIR, unreal.StageSpanData, unreal.DataAssetFactory())
    item.set_editor_property("display_name", display_name)
    item.set_editor_property("description", description)
    item.set_editor_property("category_tag", make_tag(category))
    item.set_editor_property("item_type", item_type)
    item.set_editor_property("fit", fit)
    item.set_editor_property("segment_length", segment_length)
    item.set_editor_property("max_segments", max_segments)
    item.set_editor_property("angle_snap_degrees", 15.0)
    item.set_editor_property("segment_parts", segment_parts)
    item.set_editor_property("joint_parts", joint_parts)
    item.set_editor_property("specs", specs)
    rules = item.get_editor_property("placement_rules")
    rules.set_editor_property("grid_size", unreal.Vector(grid, grid, 0.0))
    item.set_editor_property("placement_rules", rules)
    eal.save_loaded_asset(item)

    saved = eal.load_asset(ITEMS_DIR + "/" + name)
    log("Saved {}: {} segment parts, {} joint parts, actor class {}".format(name, len(saved.get_editor_property("segment_parts")),
        len(saved.get_editor_property("joint_parts")), saved.get_editor_property("actor_class")))


def create_truss(cylinder, cube, metal, coupler):
    # F34-class box truss on the floor: 29 cm outer width, 5 cm chords, 3 cm lacing in four 50 cm bays per 2 m section.
    length, half_width, bottom, top = 200.0, 12.0, 2.5, 26.5
    parts = []
    for y in (-half_width, half_width):
        for z in (bottom, top):
            parts.append(tube_along_x(cylinder, metal, (length / 2.0, y, z), length, 5.0))

    bay = 50.0
    rise = top - bottom
    diagonal = math.hypot(bay, rise)
    angle = math.degrees(math.atan2(rise, bay))
    for index in range(4):
        x = bay * index + bay / 2.0
        zigzag = angle if index % 2 == 0 else -angle
        # Side faces: the lacing climbs and falls in the XZ plane.
        for y in (-half_width, half_width):
            parts.append(part(cylinder, metal, (x, y, (bottom + top) / 2.0), (90.0 + zigzag, 0.0, 0.0), (0.03, 0.03, diagonal / 100.0)))
        # Top and bottom faces: the lacing zigzags across in the XY plane.
        width_diagonal = math.hypot(bay, 2.0 * half_width)
        width_angle = math.degrees(math.atan2(2.0 * half_width, bay)) * (1 if index % 2 == 0 else -1)
        for z in (bottom, top):
            parts.append(part(cylinder, metal, (x, 0.0, z), (90.0, width_angle, 0.0), (0.03, 0.03, width_diagonal / 100.0)))

    joints = [part(cube, coupler, (0.0, 0.0, (bottom + top) / 2.0), scale=(0.03, 0.30, 0.30))]
    save_span("DA_Span_TrussF34", "F34 Truss Run (2 m sections)",
        "Box truss drawn from a start to an end point, in whole 2 m sections. Weight is per section.",
        "StageCraft.Category.Truss.Straight", unreal.StageItemType.TRUSS, unreal.StageSpanFit.WHOLE_SEGMENTS, length, 50,
        parts, joints, make_specs("Generic", 14.0, 0.0), 50.0)


def create_railing(cylinder, cube, metal):
    segment = 100.0
    parts = [
        tube_along_x(cylinder, metal, (segment / 2.0, 0.0, 100.0), segment, 4.0),
        tube_along_x(cylinder, metal, (segment / 2.0, 0.0, 55.0), segment, 3.0),
    ]
    joints = [
        part(cylinder, metal, (0.0, 0.0, 50.0), scale=(0.05, 0.05, 1.0)),
        part(cube, metal, (0.0, 0.0, 0.5), scale=(0.15, 0.15, 0.01)),
    ]
    save_span("DA_Span_Railing", "Stage Handrail (1 m sections)",
        "Edge protection drawn from a start to an end point; sections stretch slightly to the exact length, posts at every joint.",
        "StageCraft.Category.Stage.Railing", unreal.StageItemType.STAGE_ELEMENT, unreal.StageSpanFit.STRETCH, segment, 100,
        parts, joints, make_specs("Generic", 6.0, 0.0), 25.0)


def create_cable(cylinder, cable):
    segment = 100.0
    parts = [tube_along_x(cylinder, cable, (segment / 2.0, 0.0, 1.25), segment, 2.5)]
    save_span("DA_Span_CableRun", "Power Cable Run (per metre)",
        "Floor cable drawn from a start to an end point; billed per metre.",
        "StageCraft.Category.Cable", unreal.StageItemType.PROP, unreal.StageSpanFit.STRETCH, segment, 300,
        parts, [], make_specs("Generic", 0.4, 0.0), 25.0)


create_panel("DA_Panel_Outliner", "StageCraft.Panel.Outliner", "Outliner", unreal.StageOutlinerPanel.static_class())
create_panel("DA_Panel_BillOfMaterials", "StageCraft.Panel.BillOfMaterials", "Bill of Materials", unreal.StageBillOfMaterialsPanel.static_class())

cylinder_mesh = unreal.load_asset(CYLINDER)
cube_mesh = unreal.load_asset(CUBE)
if cylinder_mesh is None or cube_mesh is None:
    raise RuntimeError("Engine BasicShapes not found")

metal_material = create_material("M_SpanMetal", (0.55, 0.56, 0.58), 1.0, 0.35)
coupler_material = create_material("M_SpanCoupler", (0.18, 0.18, 0.19), 1.0, 0.5)
cable_material = create_material("M_SpanCable", (0.02, 0.02, 0.02), 0.0, 0.6)

create_truss(cylinder_mesh, cube_mesh, metal_material, coupler_material)
create_railing(cylinder_mesh, cube_mesh, metal_material)
create_cable(cylinder_mesh, cable_material)
log("Done.")
