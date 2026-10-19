"""Import failures remain readable and never replace the selected model."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile
from test_zobj_import import human
from zobj_import import android
from zobj_import.errors import message


class PakErrorTests(unittest.TestCase):
    def test_android_returns_validation_error_without_success(self):
        with patch.object(android, "generate_and_install", side_effect=ValueError("Unsupported rig")):
            result = json.loads(android.import_model("user", "rom", "model", None))
        self.assertEqual(result, {"error": "Unsupported rig"})

    def test_system_and_unexpected_errors_hide_internal_details(self):
        for error in (OSError("private filename"), RuntimeError("private filename")):
            self.assertNotIn("private filename", message(error))
            self.assertNotIn("Traceback", message(error))

    def test_desktop_reports_rejected_model_without_traceback(self):
        try:
            from zobj_import import convert
        except ImportError as error:
            self.skipTest(str(error))
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            package = root / "unsupported.pak"
            model = human()
            model[0x500B] = 3
            with zipfile.ZipFile(package, "w") as archive:
                archive.writestr("player.zobj", model)
            sentinel = root / "user/sdmc/save.bin"
            sentinel.parent.mkdir(parents=True)
            sentinel.write_bytes(b"preserved save")
            result = subprocess.run([
                sys.executable, str(Path(__file__).with_name("import_pak.py")),
                "--root", str(root), "--package", str(root),
                "--mm-rom", str(root / "missing.3ds"), "--model", str(package),
            ], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 1)
            report = json.loads(result.stdout.splitlines()[-1])
            self.assertIn("human Link", report["error"])
            self.assertNotIn("Traceback", result.stdout + result.stderr)
            self.assertEqual(sentinel.read_bytes(), b"preserved save")
