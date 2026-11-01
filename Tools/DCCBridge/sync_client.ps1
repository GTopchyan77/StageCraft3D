# Copies the bridge client (the single source in common/) next to every add-on, which loads its own copy
# (Docs/ADR/0022-dcc-bridge.md). Run after editing common/stagecraft_bridge_client.py.
$ErrorActionPreference = 'Stop'
$Source = Join-Path $PSScriptRoot 'common\stagecraft_bridge_client.py'
foreach ($Folder in @('blender\stagecraft_bridge', 'cinema4d\stagecraft_bridge', '3dsmax', 'maya')) {
    Copy-Item $Source (Join-Path $PSScriptRoot $Folder) -Force
    Write-Host "Copied to $Folder"
}
