"""StageCraft Bridge for Blender (Docs/ADR/0022-dcc-bridge.md).

Receives stages sent from StageCraft 3D (File > Send to 3D App) and imports them into the open .blend, each into its own
collection. Install: Edit > Preferences > Add-ons > Install... and pick a zip of this folder (or copy the folder into Blender's
scripts/addons). The panel is in the 3D Viewport sidebar (N), tab "StageCraft".
"""

bl_info = {
    "name": "StageCraft Bridge",
    "author": "StageCraft 3D",
    "version": (1, 0, 0),
    "blender": (3, 6, 0),
    "location": "View3D > Sidebar > StageCraft",
    "description": "Receive stages from StageCraft 3D running on this computer",
    "category": "Import-Export",
}

import bpy

from . import stagecraft_bridge_client as bridge

# Polling a non-blocking socket from a timer is how a Blender add-on listens: Blender has no socket events for Python.
POLL_SECONDS = 0.25

_client = None


def import_glb(path, name):
    """Imports path with Blender's own glTF importer into a new collection "StageCraft - <name>". Raises on failure."""
    before = set(bpy.data.objects)
    result = bpy.ops.import_scene.gltf(filepath=path)
    if "FINISHED" not in result:
        raise RuntimeError("Blender's glTF importer did not finish")
    created = [obj for obj in bpy.data.objects if obj not in before]
    collection = bpy.data.collections.new("StageCraft - {}".format(name))
    bpy.context.scene.collection.children.link(collection)
    for obj in created:
        for owner in list(obj.users_collection):
            owner.objects.unlink(obj)
        collection.objects.link(obj)
    return len(created)


def _redraw_panels():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == "VIEW_3D":
                area.tag_redraw()


def _tick():
    if _client is None:
        return None
    _client.pump(import_glb)
    _redraw_panels()
    # Returning None ends the timer: nothing more can arrive once StageCraft has closed the connection.
    return POLL_SECONDS if _client.connected else None


class STAGECRAFT_OT_connect(bpy.types.Operator):
    bl_idname = "stagecraft.bridge_connect"
    bl_label = "Connect to StageCraft"
    bl_description = "Connect to StageCraft 3D on this computer (switch on \"Allow 3D apps to connect\" in StageCraft first)"

    def execute(self, context):
        global _client
        if _client is None:
            _client = bridge.BridgeClient("blender", ".".join(str(part) for part in bpy.app.version))
        try:
            _client.connect()
        except bridge.BridgeError as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}
        if not bpy.app.timers.is_registered(_tick):
            bpy.app.timers.register(_tick, first_interval=POLL_SECONDS)
        return {"FINISHED"}


class STAGECRAFT_OT_disconnect(bpy.types.Operator):
    bl_idname = "stagecraft.bridge_disconnect"
    bl_label = "Disconnect"
    bl_description = "Close the connection to StageCraft"

    def execute(self, context):
        if _client is not None:
            _client.close()
        return {"FINISHED"}


class STAGECRAFT_PT_bridge(bpy.types.Panel):
    bl_label = "StageCraft Bridge"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "StageCraft"

    def draw(self, context):
        layout = self.layout
        connected = _client is not None and _client.connected
        layout.label(text=_client.status if _client else "Not connected", icon="LINKED" if connected else "UNLINKED")
        if connected:
            layout.operator(STAGECRAFT_OT_disconnect.bl_idname, icon="CANCEL")
        else:
            layout.operator(STAGECRAFT_OT_connect.bl_idname, icon="PLUGIN")


CLASSES = (STAGECRAFT_OT_connect, STAGECRAFT_OT_disconnect, STAGECRAFT_PT_bridge)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)


def unregister():
    global _client
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)
    if _client is not None:
        _client.close()
        _client = None
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
