"""StageCraft Bridge for 3ds Max (Docs/ADR/0022-dcc-bridge.md).

Receives stages sent from StageCraft 3D (File > Send to 3D App) and imports them into the open scene with 3ds Max's glTF importer
(versions that include one). Run it once per session with Scripting > Run Script... (keep stagecraft_bridge_client.py next to this
file); it opens a small window to connect and disconnect.

Status: written against the documented pymxs and Qt APIs but not yet run in 3ds Max (no copy on the development machine); see
STATE.md.
"""

import os
import sys

from pymxs import runtime as rt

try:
    from PySide6 import QtCore, QtWidgets
except ImportError:  # 3ds Max 2024 and older ship PySide2.
    from PySide2 import QtCore, QtWidgets

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stagecraft_bridge_client as bridge  # noqa: E402

POLL_MILLISECONDS = 250


def import_glb(path, name):
    # importFile picks the importer by extension; it returns false when this 3ds Max has no glTF importer.
    if not rt.importFile(path, rt.Name("noPrompt")):
        raise RuntimeError("3ds Max could not import the glTF file (this version may have no glTF importer)")
    rt.redrawViews()


class BridgeWindow(QtWidgets.QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("StageCraft Bridge")
        self.client = bridge.BridgeClient("max", str(rt.maxversion()[0]))  # release number, e.g. 27000 for 3ds Max 2025
        self.status = QtWidgets.QLabel("Not connected")
        self.button = QtWidgets.QPushButton("Connect to StageCraft")
        self.button.clicked.connect(self.toggle)
        layout = QtWidgets.QVBoxLayout(self)
        layout.addWidget(self.status)
        layout.addWidget(self.button)
        # A Qt timer runs on 3ds Max's main thread, where scene changes are allowed.
        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self.tick)

    def toggle(self):
        if self.client.connected:
            self.client.close()
        else:
            try:
                self.client.connect()
            except bridge.BridgeError as error:
                self.status.setText(str(error))
                return
            self.timer.start(POLL_MILLISECONDS)
        self.refresh()

    def tick(self):
        self.client.pump(import_glb)
        if not self.client.connected:
            self.timer.stop()
        self.refresh()

    def refresh(self):
        self.status.setText(self.client.status)
        self.button.setText("Disconnect" if self.client.connected else "Connect to StageCraft")

    def closeEvent(self, event):
        self.timer.stop()
        self.client.close()
        super().closeEvent(event)


_window = BridgeWindow(QtWidgets.QWidget.find(rt.windows.getMAXHWND()))
_window.setWindowFlags(QtCore.Qt.Tool)
_window.show()
