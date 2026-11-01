"""StageCraft Bridge for Maya (Docs/ADR/0022-dcc-bridge.md).

Maya has no built-in glTF importer, and bridge protocol 1 sends glTF binary only, so this add-on connects and shows the state but
StageCraft does not send stages to Maya yet: that arrives with the USD writer (ADR 0022 §8, phase D2), which Maya imports through
its bundled Maya USD plug-in. Until then, save a .glb from StageCraft and import it with a glTF plug-in for Maya.

Run in the Script Editor (Python): import stagecraft_bridge; stagecraft_bridge.show()   (with this folder on the Python path).

Status: written against the documented maya.cmds and Qt APIs but not yet run in Maya (no copy on the development machine); see
STATE.md.
"""

import maya.cmds as cmds

try:
    from PySide6 import QtCore
except ImportError:  # Maya 2024 and older ship PySide2.
    from PySide2 import QtCore

import stagecraft_bridge_client as bridge

POLL_MILLISECONDS = 500
WINDOW = "stageCraftBridgeWindow"

_client = bridge.BridgeClient("maya", cmds.about(version=True))
_timer = QtCore.QTimer()


def _refuse(path, name):
    raise RuntimeError("Maya imports StageCraft stages once the USD export exists; save a .glb from StageCraft instead")


def _tick():
    _client.pump(_refuse)
    if not _client.connected:
        _timer.stop()
    _refresh()


def _refresh():
    if cmds.window(WINDOW, exists=True):
        cmds.text("stageCraftBridgeStatus", edit=True, label=_client.status)
        cmds.button("stageCraftBridgeButton", edit=True, label="Disconnect" if _client.connected else "Connect to StageCraft")


def _toggle(*_):
    if _client.connected:
        _client.close()
    else:
        try:
            _client.connect()
        except bridge.BridgeError as error:
            cmds.warning(str(error))
            return
        _timer.start(POLL_MILLISECONDS)
    _refresh()


def show():
    if cmds.window(WINDOW, exists=True):
        cmds.deleteUI(WINDOW)
    cmds.window(WINDOW, title="StageCraft Bridge", widthHeight=(320, 90))
    cmds.columnLayout(adjustableColumn=True, rowSpacing=6)
    cmds.text("stageCraftBridgeStatus", label=_client.status, align="left")
    cmds.button("stageCraftBridgeButton", label="Connect to StageCraft", command=_toggle)
    cmds.text(label="Maya: sending arrives with USD export (coming).", align="left")
    cmds.showWindow(WINDOW)


_timer.timeout.connect(_tick)
