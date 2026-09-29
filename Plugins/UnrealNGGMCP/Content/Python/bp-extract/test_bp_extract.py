# Copyright 2025-2026 NGG. All Rights Reserved.
"""Offline smoke tests for bp-extract (stdlib unittest, no editor / no bridge).

Run from inside this folder so the flat sibling imports resolve:

    py Plugins\\UnrealNGGMCP\\Tools\\bp-extract\\test_bp_extract.py
    # or:  cd Plugins\\UnrealNGGMCP\\Tools\\bp-extract && py -m unittest

Covers the de-hardcoding (custom mirror token / prefixes), path mapping, dependency
classification, ordering, and config-resolution precedence.
"""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

# Running this file directly puts its own dir on sys.path[0]; make `-m unittest` from
# elsewhere work too.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import config
import typemap
import paths
import deps
import order


def _full_config(**overrides):
    base = dict(
        project_root=Path("/tmp/proj"),
        module_name="MyGame",
        api_macro="MYGAME_API",
        source_dir=Path("/tmp/proj/Source/MyGame"),
        mirror_token="MyGame",
        name_prefixes=config.DEFAULT_NAME_PREFIXES,
        class_group="MyGame",
        vault_dir=Path("/tmp/proj/BlueprintConvertInfo"),
        out_dir=Path("/tmp/proj/BlueprintConvertInfo/_extracted"),
        briefs_dir=Path("/tmp/proj/BlueprintConvertInfo/_briefs"),
        ini_path=Path("/tmp/proj/Config/DefaultEngine.ini"),
        bridge_url=None,
    )
    base.update(overrides)
    return config.Config(**base)


def _args(**overrides):
    """A fake argparse namespace where every config flag defaults to None."""
    defaults = dict(
        config=None, project_root=None, module=None, api_macro=None, token=None,
        class_group=None, prefixes=None, vault=None, out=None, briefs=None, ini=None,
        bridge_url=None, print_config=False,
    )
    defaults.update(overrides)
    return SimpleNamespace(**defaults)


class DeHardcodingTests(unittest.TestCase):
    def tearDown(self):
        # Restore module globals so a later real run / test isn't poisoned.
        typemap.TOKEN = "GameAnim"
        typemap._NAME_PREFIXES = config.DEFAULT_NAME_PREFIXES

    def test_mirror_token_is_configurable(self):
        config.configure(_full_config(mirror_token="ZZ"))
        self.assertEqual(typemap.enum_mirror("/Game/Foo/E_Bar"), "EZZBar")
        self.assertEqual(typemap.struct_mirror("/Game/Foo/S_Bar"), "FZZBar")
        self.assertEqual(typemap.class_mirror("/Game/Foo/BP_Baz", actor=True), "AZZBaz")
        self.assertEqual(typemap.class_mirror("/Game/Foo/BP_Baz", actor=False), "UZZBaz")
        self.assertEqual(typemap.interface_mirror("/Game/Foo/BPI_Qux"),
                         ("UZZQuxInterface", "IZZQuxInterface"))

    def test_name_prefixes_are_configurable(self):
        # Only BP_ stripped: E_Bar keeps its E.
        config.configure(_full_config(mirror_token="ZZ", name_prefixes=("BP_",)))
        self.assertEqual(typemap.clean_name("BP_Foo"), "Foo")
        self.assertEqual(typemap.clean_name("E_Bar"), "EBar")
        # Default prefixes DO strip E_.
        config.configure(_full_config(mirror_token="ZZ"))
        self.assertEqual(typemap.clean_name("E_Bar"), "Bar")


class PathTests(unittest.TestCase):
    def test_to_object_path(self):
        self.assertEqual(paths.to_object_path("Content/Foo/Bar.uasset"), "/Game/Foo/Bar")
        self.assertEqual(paths.to_object_path("Content/Foo/Baz.umap"), "/Game/Foo/Baz")
        self.assertEqual(paths.to_object_path("/Game/Foo/Bar"), "/Game/Foo/Bar")
        self.assertEqual(paths.to_object_path("/Script/Engine.Actor"), "/Script/Engine.Actor")
        self.assertEqual(paths.asset_name("/Game/Foo/Bar.Bar_C"), "Bar_C")


class DepsTests(unittest.TestCase):
    def test_classify(self):
        self.assertEqual(deps.classify("/Script/Engine.Actor"), "engine")
        self.assertEqual(deps.classify("/Engine/Foo"), "engine")
        self.assertEqual(deps.classify("/Game/Foo"), "project")
        self.assertEqual(deps.classify(""), "other")

    def test_asset_dependencies_splits_engine_and_project(self):
        record = {
            "asset": {"object_path": "/Game/A", "parent_class": "/Script/Engine.Actor"},
            "interfaces": [],
            "variables": [{"type": "struct:/Game/B"}],
            "components": [],
        }
        d = deps.asset_dependencies(record)
        self.assertIn("/Script/Engine.Actor", d["engine"])
        self.assertIn("/Game/B", d["project"])
        self.assertNotIn("/Game/A", d["project"])  # self excluded


class OrderTests(unittest.TestCase):
    @staticmethod
    def _rec(path, kind, dep=None):
        variables = [{"type": f"struct:{dep}"}] if dep else []
        return {"asset": {"object_path": path, "name": path.rsplit("/", 1)[-1],
                          "asset_kind": kind}, "variables": variables,
                "interfaces": [], "components": [], "functions": []}

    def test_tiers_and_no_cycle(self):
        recs = [self._rec("/Game/A", "Blueprint", dep="/Game/B"),
                self._rec("/Game/B", "Struct")]
        result = order.build_order(recs)
        self.assertEqual(result["tier"]["/Game/B"], 0)
        self.assertEqual(result["tier"]["/Game/A"], 1)
        self.assertEqual(result["cycles"], [])

    def test_cycle_detection(self):
        recs = [self._rec("/Game/A", "Blueprint", dep="/Game/B"),
                self._rec("/Game/B", "Blueprint", dep="/Game/A")]
        result = order.build_order(recs)
        self.assertEqual(len(result["cycles"]), 1)
        self.assertEqual(sorted(result["cycles"][0]), ["/Game/A", "/Game/B"])


class ConfigPrecedenceTests(unittest.TestCase):
    def setUp(self):
        self._saved = {k: os.environ.get(k) for k in
                       ("BPX_MODULE", "BPX_TOKEN", "BPX_API_MACRO", "BPX_SOURCE", "BPX_VAULT", "BPX_PROJECT_ROOT")}
        for k in self._saved:
            os.environ.pop(k, None)

    def tearDown(self):
        for k, v in self._saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v

    def test_cli_beats_env_and_file(self):
        os.environ["BPX_MODULE"] = "ENVMOD"
        cfg = config.resolve(_args(module="CLIMOD", project_root="/tmp/proj"))
        self.assertEqual(cfg.module_name, "CLIMOD")

    def test_env_beats_file(self):
        os.environ["BPX_MODULE"] = "ENVMOD"
        cfg = config.resolve(_args(project_root="/tmp/proj"))
        self.assertEqual(cfg.module_name, "ENVMOD")

    def test_file_beats_fallback(self):
        with tempfile.TemporaryDirectory() as d:
            cfgfile = Path(d) / config.CONFIG_FILENAME
            cfgfile.write_text('{"module_name": "FILEMOD", "mirror_token": "FT"}', encoding="utf-8")
            cfg = config.resolve(_args(config=str(cfgfile)))
            self.assertEqual(cfg.module_name, "FILEMOD")
            self.assertEqual(cfg.mirror_token, "FT")
            self.assertEqual(cfg.api_macro, "FILEMOD_API")  # derived from the file's module

    def test_api_macro_derived_from_module(self):
        cfg = config.resolve(_args(module="CoolGame", project_root="/tmp/proj"))
        self.assertEqual(cfg.api_macro, "COOLGAME_API")

    def test_token_falls_back_to_module(self):
        cfg = config.resolve(_args(module="CoolGame", project_root="/tmp/proj"))
        self.assertEqual(cfg.mirror_token, "CoolGame")
        self.assertEqual(cfg.class_group, "CoolGame")


if __name__ == "__main__":
    unittest.main(verbosity=2)
