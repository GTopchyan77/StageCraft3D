# ue5_meshy_generate — Meshy.ai text-to-3D integration

Generates a 3D mesh from a text prompt using the Meshy.ai v2 API, saves the
file under `RawAssets/Meshy/`, then imports it into the UE5 content browser as
a Static Mesh via the existing `/assets/import` plugin route.

## Get an API key

1. Sign up at https://www.meshy.ai
2. Open https://www.meshy.ai/settings/api and copy your key (it starts with `msy-`).

## Provide the key (pick one)

- **Env var (recommended):** `set MESHY_API_KEY=msy-...` (Windows) / `export MESHY_API_KEY=msy-...`
- **Config file:** create `<project_root>/.meshy.json` with:
  ```json
  {"api_key":"msy-..."}
  ```
  The path `.meshy.json` is gitignored. Never commit this file.

Env var takes priority over the file.

## Example call

```
ue5_meshy_generate(
  prompt="a chunky cartoon cannon with brass trim",
  art_style="realistic",
  asset_name="SM_Cannon",
  dest_path="/Game/MyGame/Meshes",
  refine=false
)
```

Result includes the task id, local file path, UE5 asset path, and tri count if
the plugin reports one.

## Expected latency

- Preview: 1-3 minutes
- Refine (optional): additional 2-5 minutes

The tool polls every 10 seconds, with a 15-minute hard timeout per phase.

## Triangle count

Preview meshes are roughly 30k-50k tris. Refine keeps the topology but adds
PBR textures and tightens surface detail.

## Cost (as of 2025)

Roughly 20 credits per preview and 10 credits per refine. A $20 pack buys
around 200 previews.
