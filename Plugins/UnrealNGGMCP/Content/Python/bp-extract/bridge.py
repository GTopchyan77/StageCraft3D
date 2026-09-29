# Copyright 2025-2026 NGG. All Rights Reserved.
"""Thin HTTP client for the UnrealNGGMCP editor bridge (read-only routes).

The bridge is the local HTTP server exposed by the UnrealNGGMCP editor plugin
(default http://localhost:6776). This client only ever issues GET requests, so
it cannot mutate or save any asset.

Configuration mirrors the Node client (Source/ThirdParty/unrealngg-mcp/ue5client.js):
  - Base URL:  $NGG_BRIDGE_URL   (default http://localhost:6776)
  - Token:     $NGG_BRIDGE_TOKEN, else [UnrealNGGMCP] AuthToken in
               Config/DefaultEngine.ini (empty = auth disabled)

Stdlib only — no third-party dependencies.
"""

from __future__ import annotations

import json
import os
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

DEFAULT_BRIDGE_URL = "http://localhost:6776"


class BridgeError(RuntimeError):
    """Raised for any bridge transport or HTTP error, with a human-readable hint."""


def _token_from_ini(ini_path: Path) -> str | None:
    """Parse [UnrealNGGMCP] AuthToken from a UE .ini without configparser.

    UE .ini files use ``+Key=`` accumulator syntax and contain ``%`` and other
    characters that trip configparser's interpolation, so we scan by hand.
    """
    try:
        text = ini_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None

    in_section = False
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("[") and line.endswith("]"):
            in_section = line[1:-1].strip().lower() == "unrealnggmcp"
            continue
        if in_section and "=" in line:
            key, _, value = line.partition("=")
            if key.strip().lower() == "authtoken":
                value = value.strip()
                return value or None
    return None


class Bridge:
    """Read-only client for the UnrealNGGMCP HTTP bridge."""

    def __init__(
        self,
        base_url: str | None = None,
        token: str | None = None,
        ini_path: Path | None = None,
        timeout: float = 30.0,
    ) -> None:
        self.base_url = (base_url or os.environ.get("NGG_BRIDGE_URL") or DEFAULT_BRIDGE_URL).rstrip("/")
        self.timeout = timeout

        if token is not None:
            self.token: str | None = token or None
        elif os.environ.get("NGG_BRIDGE_TOKEN"):
            self.token = os.environ["NGG_BRIDGE_TOKEN"]
        elif ini_path is not None:
            self.token = _token_from_ini(ini_path)
        else:
            self.token = None

    # -- transport ----------------------------------------------------------

    def _get(self, path: str, params: dict[str, str] | None = None) -> dict:
        url = self.base_url + path
        if params:
            url += "?" + urllib.parse.urlencode(params)

        headers = {"Accept": "application/json"}
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"

        req = urllib.request.Request(url, headers=headers, method="GET")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                raw = resp.read().decode("utf-8", errors="replace")
        except urllib.error.HTTPError as err:
            body = err.read().decode("utf-8", errors="replace") if err.fp else ""
            message = body
            try:
                message = json.loads(body).get("error", body)
            except (ValueError, AttributeError):
                pass
            if err.code == 401:
                message += " (set NGG_BRIDGE_TOKEN to match [UnrealNGGMCP] AuthToken)"
            raise BridgeError(f"HTTP {err.code} from {path}: {message}") from err
        except urllib.error.URLError as err:
            raise BridgeError(
                f"Cannot reach the bridge at {self.base_url} ({err.reason}). "
                "Is the Unreal Editor open with the UnrealNGGMCP plugin loaded?"
            ) from err

        try:
            return json.loads(raw)
        except ValueError as err:
            raise BridgeError(f"Non-JSON response from {path}: {raw[:200]!r}") from err

    # -- routes -------------------------------------------------------------

    def health(self) -> dict:
        return self._get("/health")

    def project_info(self) -> dict:
        return self._get("/project_info")

    def assets_list(self, content_path: str) -> dict:
        return self._get("/assets/list", {"path": content_path})

    def assets_get(self, object_path: str) -> dict:
        return self._get("/assets/get", {"path": object_path})

    def bp_list_variables(self, blueprint: str) -> dict:
        return self._get("/bp/list_variables", {"blueprint": blueprint})

    def bp_read_graph(self, blueprint: str, graph: str | None = None) -> dict:
        params = {"blueprint": blueprint}
        if graph:
            params["graph"] = graph
        return self._get("/bp/read_graph", params)
