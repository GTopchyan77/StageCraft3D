# StageCraft DCC Bridge add-ons

These add-ons send your saved StageCraft stages into Blender, 3ds Max and Cinema 4D running on the same computer. Maya follows once USD export exists. The design is in Docs/ADR/0022-dcc-bridge.md.

## How it works

1. In StageCraft, open **File › Send to 3D App…** and switch on **Allow 3D apps to connect**.
2. In your 3D app, click the add-on's **Connect**.
3. In StageCraft, pick a saved scene and click the app's button. The stage is imported into the open file.

Things to know:

- **What you can send:** only your own stages. Stages from the Community Marketplace stay in StageCraft, and items you don't own are left out.
- **Format:** stages travel as glTF binary (`.glb`). **Save as .glb file…** in the same window writes one to disk for any other app.
- **Connections:** only programs on this computer can connect. They need the token StageCraft writes to `%LOCALAPPDATA%\StageCraft\Bridge\session.json` while the switch is on.

## Install

The add-ons share `common/stagecraft_bridge_client.py`. Every add-on folder holds a copy; after editing the one in `common/`, run `sync_client.ps1`.

| App | Install | Use |
|---|---|---|
| Blender 3.6+ | Zip the `blender/stagecraft_bridge` folder, then Edit › Preferences › Add-ons › Install… and enable "StageCraft Bridge" | 3D Viewport sidebar (N) › StageCraft › Connect |
| Cinema 4D 2023+ | Copy `cinema4d/stagecraft_bridge` into Cinema 4D's `plugins` folder and restart | Extensions › StageCraft Bridge (toggles the connection) |
| 3ds Max (with glTF import) | Keep `stagecraft_bridge.py` and `stagecraft_bridge_client.py` together | Scripting › Run Script… › `3dsmax/stagecraft_bridge.py` |
| Maya | Put the `maya` folder on the Python path | Script Editor: `import stagecraft_bridge; stagecraft_bridge.show()` (connects; importing waits for USD) |

**Status:** the Blender add-on is verified end to end (headless Blender 4.3, `tests/blender_receive.py`). The Cinema 4D, 3ds Max and Maya add-ons follow those apps' documented APIs but have not been run yet.

**Cinema 4D:** the plug-in IDs are from Maxon's development range. Register your own at developers.maxon.net before you distribute the plug-in.

## Test

With StageCraft running and the bridge switched on (for example `StageCraft.Bridge.Start` in a development build):

```
blender -b --factory-startup --python Tools/DCCBridge/tests/blender_receive.py -- 90 result.json
```

Then send a scene to it (`StageCraft.Bridge.Send first <scene>`). The script checks the client's file checks, that a wrong token is refused, and that the stage imports.
