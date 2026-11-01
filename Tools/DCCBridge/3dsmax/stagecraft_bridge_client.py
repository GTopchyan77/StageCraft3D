"""StageCraft DCC bridge client, protocol 1 (Docs/ADR/0022-dcc-bridge.md).

The other end is Source/ModularSceneBuilder/Private/Bridge/StageBridgeProtocol.h; the two change together.
This file is the single source: every add-on folder holds an identical copy (Tools/DCCBridge/sync_client.ps1 copies it, and the
verification run checks that the copies match). Standard library only, Python 3.7+, so it runs inside Blender, Maya, 3ds Max
and Cinema 4D unchanged.

How a session works:
  1. StageCraft's "Allow 3D apps to connect" switch writes a session file for this user: port, token and outbox folder.
  2. connect() reads it, opens 127.0.0.1:<port> and says hello with the token.
  3. The app's timer calls pump() a few times a second. Each "asset" message names a .glb file in the outbox; it is checked
     (inside the outbox, the size and SHA-1 StageCraft announced) before the app's importer sees it, and the result is acked.
Nothing here ever runs code from StageCraft or opens a file outside the outbox.
"""

import hashlib
import json
import os
import socket

PROTOCOL = 1
MAX_LINE_BYTES = 64 * 1024
CONNECT_TIMEOUT_SECONDS = 2.0
MAX_ERROR_LENGTH = 200


class BridgeError(Exception):
    """A user-facing reason: no session, refused connection, a file that fails its checks."""


def session_file_path():
    """Where StageCraft writes the session: FPlatformProcess::UserSettingsDir()/StageCraft/Bridge/session.json."""
    base = os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "StageCraft", "Bridge", "session.json")


def read_session(path=None):
    path = path or session_file_path()
    try:
        with open(path, "r", encoding="utf-8") as handle:
            session = json.load(handle)
    except FileNotFoundError:
        raise BridgeError("StageCraft is not accepting connections. In StageCraft, open File > Send to 3D App and switch on "
                          "\"Allow 3D apps to connect\".")
    except (OSError, ValueError) as error:
        raise BridgeError("The StageCraft session file could not be read: {}".format(error))
    if not isinstance(session, dict) or session.get("protocol") != PROTOCOL:
        raise BridgeError("This add-on speaks bridge protocol {}; update the add-on or StageCraft.".format(PROTOCOL))
    for key in ("port", "token", "outbox"):
        if key not in session:
            raise BridgeError("The StageCraft session file is incomplete (no {}).".format(key))
    return session


def is_inside(path, directory):
    """True when path resolves (links and '..' followed) to a file inside directory."""
    real_path = os.path.normcase(os.path.realpath(path))
    real_directory = os.path.normcase(os.path.realpath(directory))
    try:
        return os.path.commonpath([real_path, real_directory]) == real_directory and real_path != real_directory
    except ValueError:
        # Different drives on Windows.
        return False


def verify_asset(message, outbox):
    """Returns the checked path of an asset message's file, or raises BridgeError with the reason."""
    if message.get("format") != "glb":
        raise BridgeError("Unsupported format {!r}; this add-on imports glb.".format(message.get("format")))
    path = message.get("path")
    if not isinstance(path, str) or not path.lower().endswith(".glb"):
        raise BridgeError("The asset has no .glb path.")
    if not is_inside(path, outbox):
        raise BridgeError("Refused a file outside the StageCraft outbox.")
    if not os.path.isfile(path):
        raise BridgeError("The file is gone from the outbox.")
    expected_bytes = message.get("bytes")
    if not isinstance(expected_bytes, (int, float)) or os.path.getsize(path) != int(expected_bytes):
        raise BridgeError("The file size does not match what StageCraft sent.")
    digest = hashlib.sha1()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    if digest.hexdigest() != str(message.get("sha1", "")).lower():
        raise BridgeError("The file does not match its checksum.")
    return path


class BridgeClient:
    """One connection to StageCraft. Not thread-safe: use it from the app's main thread (its timer)."""

    def __init__(self, app, app_version):
        self.app = app
        self.app_version = str(app_version)[:32]
        self.client_id = 0
        self.status = "Not connected"
        self._socket = None
        self._pending = b""
        self._outbox = None

    @property
    def connected(self):
        return self._socket is not None

    def connect(self, session_path=None):
        self.close()
        session = read_session(session_path)
        try:
            sock = socket.create_connection(("127.0.0.1", int(session["port"])), timeout=CONNECT_TIMEOUT_SECONDS)
        except OSError as error:
            raise BridgeError("Could not reach StageCraft on port {}: {}".format(session["port"], error))
        sock.setblocking(False)
        self._socket = sock
        self._outbox = session["outbox"]
        self._pending = b""
        self.status = "Connecting..."
        self._send({"type": "hello", "protocol": PROTOCOL, "token": session["token"], "app": self.app, "version": self.app_version})

    def close(self, status="Not connected"):
        if self._socket is not None:
            try:
                self._socket.close()
            except OSError:
                pass
        self._socket = None
        self.client_id = 0
        self.status = status

    def ack(self, asset_id, ok, error=""):
        message = {"type": "ack", "id": asset_id, "ok": bool(ok)}
        if not ok:
            message["error"] = str(error)[:MAX_ERROR_LENGTH]
        self._send(message)

    def poll(self):
        """Reads what has arrived without blocking. Returns the asset messages; handles welcome and bye itself."""
        assets = []
        if self._socket is None:
            return assets
        while True:
            try:
                data = self._socket.recv(16 * 1024)
            except (BlockingIOError, InterruptedError):
                break
            except OSError as error:
                self.close("Disconnected: {}".format(error))
                return assets
            if not data:
                self.close("StageCraft closed the connection.")
                return assets
            self._pending += data
            if len(self._pending) > MAX_LINE_BYTES and b"\n" not in self._pending:
                self.close("StageCraft sent an over-long message.")
                return assets
        while b"\n" in self._pending:
            raw, self._pending = self._pending.split(b"\n", 1)
            try:
                message = json.loads(raw.decode("utf-8"))
            except ValueError:
                continue
            kind = message.get("type") if isinstance(message, dict) else None
            if kind == "welcome":
                self.client_id = int(message.get("client", 0))
                self.status = "Connected to StageCraft"
            elif kind == "bye":
                self.close("StageCraft closed the connection: {}".format(message.get("reason", "")))
                break
            elif kind == "asset":
                assets.append(message)
        return assets

    def pump(self, importer):
        """poll(), then for each asset: verify, importer(path, name), ack. Returns how many assets were imported."""
        imported = 0
        for asset in self.poll():
            asset_id = str(asset.get("id", ""))
            name = str(asset.get("name", "StageCraft stage"))[:64]
            try:
                path = verify_asset(asset, self._outbox)
                importer(path, name)
            except Exception as error:  # The app's importer can raise anything; the reason goes back to StageCraft.
                self.ack(asset_id, False, error)
                self.status = "Import of \"{}\" failed: {}".format(name, error)
                continue
            self.ack(asset_id, True)
            self.status = "Imported \"{}\"".format(name)
            imported += 1
        return imported

    def _send(self, message):
        if self._socket is None:
            return
        line = (json.dumps(message, separators=(",", ":")) + "\n").encode("utf-8")
        try:
            # Messages are a few hundred bytes; on a loopback socket they go out at once.
            self._socket.setblocking(True)
            self._socket.sendall(line)
        except OSError as error:
            self.close("Disconnected: {}".format(error))
        finally:
            if self._socket is not None:
                self._socket.setblocking(False)
