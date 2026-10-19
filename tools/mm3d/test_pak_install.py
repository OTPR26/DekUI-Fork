"""Synthetic install transactions: preserve existing models, settings and saves."""

import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from zobj_import.install import ACTOR, install_stage, generate_and_install


class PakInstallTests(unittest.TestCase):
    def stage(self, root, data=b"new model"):
        parent = root / "load/mm3d-models"
        parent.mkdir(parents=True, exist_ok=True)
        stage = Path(tempfile.mkdtemp(prefix=".pak-import-", dir=parent))
        (stage / ACTOR).parent.mkdir(parents=True)
        (stage / ACTOR).write_bytes(data)
        (stage / "import.json").write_text(
            json.dumps({"schema": 1, "kind": "pak-import"})
        )
        return stage

    def test_reimport_keeps_previous_and_other_data(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name in (
                "load/mm3d-models/adult-link/keep",
                "config/keep",
                "sdmc/save",
            ):
                p = root / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(b"original")
            install_stage(root, self.stage(root, b"first"))
            install_stage(root, self.stage(root, b"second"))
            parent = root / "load/mm3d-models"
            self.assertEqual((parent / "pak-import" / ACTOR).read_bytes(), b"second")
            self.assertEqual(
                (parent / ".pak-import-previous" / ACTOR).read_bytes(), b"first"
            )
            for name in (
                "load/mm3d-models/adult-link/keep",
                "config/keep",
                "sdmc/save",
            ):
                self.assertEqual((root / name).read_bytes(), b"original")

    def test_install_failure_restores_previous(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            install_stage(root, self.stage(root, b"previous"))
            stage = self.stage(root)
            rename = Path.rename

            def fail_new(path, target):
                if path == stage:
                    raise OSError("rename failed")
                return rename(path, target)

            with patch.object(Path, "rename", fail_new):
                with self.assertRaises(OSError):
                    install_stage(root, stage)
            self.assertEqual(
                (root / "load/mm3d-models/pak-import" / ACTOR).read_bytes(), b"previous"
            )

    def test_incomplete_and_foreign_stages_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            stage = self.stage(root)
            (stage / ACTOR).unlink()
            with self.assertRaises(ValueError):
                install_stage(root, stage)
            foreign = root / ".pak-import-foreign"
            foreign.mkdir()
            with self.assertRaises(ValueError):
                install_stage(root, foreign)

    def test_failed_conversion_preserves_installed_model(self):
        try:
            from zobj_import import convert
        except ImportError as error:
            self.skipTest(str(error))
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            install_stage(root, self.stage(root, b"previous"))
            with patch.object(
                convert, "convert", side_effect=ValueError("unsupported model")
            ):
                with self.assertRaises(ValueError):
                    generate_and_install(root, "rom", "model")
            parent = root / "load/mm3d-models"
            self.assertEqual((parent / "pak-import" / ACTOR).read_bytes(), b"previous")
            self.assertEqual(list(parent.glob(".pak-import-*")), [])
