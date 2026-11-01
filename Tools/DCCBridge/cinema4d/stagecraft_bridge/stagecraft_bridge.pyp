"""StageCraft Bridge for Cinema 4D (Docs/ADR/0022-dcc-bridge.md).

Receives stages sent from StageCraft 3D (File > Send to 3D App) and merges them into the active document with Cinema 4D's glTF
importer (Cinema 4D 2023 or later). Install: copy this folder into Cinema 4D's plugins folder and restart. Use
Extensions > StageCraft Bridge to connect or disconnect; the status bar shows what happened.

Status: written against the documented c4d API but not yet run in Cinema 4D (no copy on the development machine); see STATE.md.
"""

import os
import sys

import c4d

sys.path.insert(0, os.path.dirname(__file__))
import stagecraft_bridge_client as bridge  # noqa: E402  (the path above makes the copy next to this file importable)

# Plugin IDs must be unique across all Cinema 4D plug-ins. 1000001-1000010 are Maxon's range for development; replace these with
# IDs registered at developers.maxon.net before distributing the plug-in.
COMMAND_ID = 1000009
TIMER_ID = 1000010
POLL_MILLISECONDS = 250

_client = bridge.BridgeClient("cinema4d", str(c4d.GetC4DVersion()))


def import_glb(path, name):
    document = c4d.documents.GetActiveDocument()
    flags = c4d.SCENEFILTER_OBJECTS | c4d.SCENEFILTER_MATERIALS
    if not c4d.documents.MergeDocument(document, path, flags, None):
        raise RuntimeError("Cinema 4D could not import the glTF file (glTF import needs Cinema 4D 2023 or later)")
    c4d.EventAdd()


class BridgeTimer(c4d.plugins.MessageData):
    """Cinema 4D calls CoreMessage on the main thread every GetTimer milliseconds while connected."""

    def GetTimer(self):
        return POLL_MILLISECONDS if _client.connected else 0

    def CoreMessage(self, message_id, bc):
        if message_id == c4d.MSG_TIMER and _client.connected:
            _client.pump(import_glb)
            c4d.StatusSetText("StageCraft: " + _client.status)
        return True


class BridgeCommand(c4d.plugins.CommandData):
    def Execute(self, document):
        if _client.connected:
            _client.close()
        else:
            try:
                _client.connect()
            except bridge.BridgeError as error:
                c4d.gui.MessageDialog(str(error))
                return True
        c4d.StatusSetText("StageCraft: " + _client.status)
        return True

    def GetState(self, document):
        return c4d.CMD_ENABLED | (c4d.CMD_VALUE if _client.connected else 0)


if __name__ == "__main__":
    c4d.plugins.RegisterCommandPlugin(COMMAND_ID, "StageCraft Bridge", 0, None, "Connect to StageCraft 3D, or disconnect", BridgeCommand())
    c4d.plugins.RegisterMessagePlugin(TIMER_ID, "StageCraft Bridge Timer", 0, BridgeTimer())
