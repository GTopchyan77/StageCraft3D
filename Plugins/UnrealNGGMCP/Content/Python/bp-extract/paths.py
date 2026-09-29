# Copyright 2025-2026 NGG. All Rights Reserved.
"""Path mapping between disk-relative content paths and UE object paths.

The Obsidian vault stores each asset's location as a disk-relative content path
(e.g. ``Content/Blueprints/Characters/SandboxCharacter_CMC.uasset``), while the
bridge's blueprint/asset routes expect a UE object path under the ``/Game`` mount
(e.g. ``/Game/Blueprints/Characters/SandboxCharacter_CMC``).
"""

from __future__ import annotations


def to_object_path(value: str) -> str:
    """Normalize a vault ``bp_path`` or a raw object path to a UE object path.

    - ``Content/Foo/Bar.uasset``  -> ``/Game/Foo/Bar``
    - ``/Game/Foo/Bar``           -> unchanged
    - ``/Script/...`` / ``/Engine/...`` -> unchanged
    """
    p = value.strip().replace("\\", "/")
    if not p:
        return p

    # Already an object path on a known mount.
    if p.startswith(("/Game", "/Script", "/Engine", "/")):
        return _strip_extension(p)

    # Disk-relative content path -> /Game mount.
    lowered = p.lower()
    idx = lowered.find("content/")
    if idx != -1:
        p = "/Game/" + p[idx + len("content/"):]
    elif not p.startswith("/Game/"):
        p = "/Game/" + p.lstrip("/")

    return _strip_extension(p)


def _strip_extension(p: str) -> str:
    for ext in (".uasset", ".umap"):
        if p.lower().endswith(ext):
            return p[: -len(ext)]
    return p


def asset_name(object_path: str) -> str:
    """Return the trailing asset name from an object path (drops any ``.Name`` suffix)."""
    tail = object_path.rstrip("/").rsplit("/", 1)[-1]
    # An object path may carry a ``Package.Asset`` suffix; keep the asset part.
    return tail.split(".", 1)[-1] if "." in tail else tail
