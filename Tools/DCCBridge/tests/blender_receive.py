"""End-to-end check of the StageCraft bridge with a real Blender (Docs/ADR/0022-dcc-bridge.md, STATE.md #53).

Run headless while StageCraft has the bridge on:
    blender -b --factory-startup --python Tools/DCCBridge/tests/blender_receive.py -- <wait seconds> <result json path> [<min lights>]

It checks, in order:
  1. the client's own refusals (paths outside the outbox, wrong size or checksum, other formats);
  2. that a connection with a wrong token is told "bye" and closed;
  3. that the add-on connects with the real token, receives one stage, imports it into a collection and acks it.
The result (including object and triangle counts) is printed as "STAGECRAFT_BLENDER_RESULT <json>" and written to the json path.
Exit code 0 only when every check passed.
"""

import hashlib
import json
import os
import socket
import sys
import tempfile
import time

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "blender"))
import stagecraft_bridge as addon  # noqa: E402
from stagecraft_bridge import stagecraft_bridge_client as bridge  # noqa: E402

args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
WAIT_SECONDS = float(args[0]) if args else 60.0
RESULT_PATH = args[1] if len(args) > 1 else os.path.join(tempfile.gettempdir(), "stagecraft_blender_result.json")
# Optional: the fewest lights the received stage must bring (its fixtures, ADR 0022 section 13). 0 skips the check.
MIN_LIGHTS = int(args[2]) if len(args) > 2 else 0

result = {"checks": {}, "imported": False}


def check(name, passed, detail=""):
    result["checks"][name] = {"passed": bool(passed), "detail": str(detail)}
    print("CHECK {} {} {}".format("PASS" if passed else "FAIL", name, detail))


def client_refusals():
    with tempfile.TemporaryDirectory() as outbox, tempfile.TemporaryDirectory() as elsewhere:
        inside = os.path.join(outbox, "a.glb")
        outside = os.path.join(elsewhere, "b.glb")
        for path in (inside, outside):
            with open(path, "wb") as handle:
                handle.write(b"glTF-test")
        sha1 = hashlib.sha1(b"glTF-test").hexdigest()
        good = {"format": "glb", "path": inside, "bytes": 9, "sha1": sha1}

        def refused(message):
            try:
                bridge.verify_asset(message, outbox)
            except bridge.BridgeError:
                return True
            return False

        check("verify accepts a good file", not refused(good))
        check("verify refuses a file outside the outbox", refused(dict(good, path=outside)))
        check("verify refuses '..' out of the outbox", refused(dict(good, path=os.path.join(outbox, "..", os.path.basename(elsewhere), "b.glb"))))
        check("verify refuses a wrong size", refused(dict(good, bytes=10)))
        check("verify refuses a wrong checksum", refused(dict(good, sha1="0" * 40)))
        check("verify refuses another format", refused(dict(good, format="usd")))


def wait_for_session():
    deadline = time.time() + WAIT_SECONDS
    while time.time() < deadline:
        try:
            return bridge.read_session()
        except bridge.BridgeError:
            time.sleep(0.5)
    return None


def wrong_token(session):
    sock = socket.create_connection(("127.0.0.1", int(session["port"])), timeout=5)
    sock.sendall((json.dumps({"type": "hello", "protocol": 1, "token": "0" * 64, "app": "blender", "version": "test"}) + "\n").encode())
    received = b""
    try:
        while True:
            data = sock.recv(4096)
            if not data:
                break
            received += data
    except OSError:
        pass
    sock.close()
    text = received.decode("utf-8", "replace")
    check("a wrong token is refused with bye", '"bye"' in text and "token" in text, text.strip())


def receive_one():
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    client = bridge.BridgeClient("blender", ".".join(str(part) for part in bpy.app.version))
    client.connect()
    deadline = time.time() + WAIT_SECONDS
    imported = 0
    while time.time() < deadline and client.connected and imported == 0:
        imported = client.pump(addon.import_glb)
        time.sleep(0.1)
    result["status"] = client.status
    result["client_id"] = client.client_id
    check("connected with the session token", client.client_id > 0, client.client_id)
    check("one stage imported and acked", imported == 1, client.status)
    # Give StageCraft a moment to read the ack before the connection closes.
    time.sleep(0.5)
    client.close()

    meshes = [obj for obj in bpy.data.objects if obj.type == "MESH"]
    triangles = 0
    for obj in meshes:
        obj.data.calc_loop_triangles()
        triangles += len(obj.data.loop_triangles)
    collections = [collection.name for collection in bpy.data.collections if collection.name.startswith("StageCraft - ")]
    result.update(imported=imported == 1, objects=len(bpy.data.objects), mesh_objects=len(meshes), triangles=triangles, collections=collections)
    check("the import has geometry", len(meshes) > 0 and triangles > 0, "{} meshes, {} triangles".format(len(meshes), triangles))
    check("the stage is in its own collection", len(collections) == 1, collections)
    lights = [obj for obj in bpy.data.objects if obj.type == "LIGHT"]
    spots = [obj for obj in lights if obj.data.type == "SPOT"]
    result.update(lights=len(lights), spot_lights=len(spots))
    if MIN_LIGHTS > 0:
        check("the fixtures' lights arrived", len(spots) >= MIN_LIGHTS, "{} lights ({} spots), at least {} expected".format(len(lights), len(spots), MIN_LIGHTS))


try:
    client_refusals()
    session = wait_for_session()
    check("StageCraft's session file appeared", session is not None)
    if session:
        wrong_token(session)
        receive_one()
except Exception as error:  # Report, never hang the run.
    check("no unexpected error", False, repr(error))

result["passed"] = all(entry["passed"] for entry in result["checks"].values())
print("STAGECRAFT_BLENDER_RESULT " + json.dumps(result))
with open(RESULT_PATH, "w", encoding="utf-8") as handle:
    json.dump(result, handle, indent=2)
sys.stdout.flush()
os._exit(0 if result["passed"] else 1)
