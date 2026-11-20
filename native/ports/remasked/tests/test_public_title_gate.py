# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

GATE = Path(__file__).resolve().parents[1] / "cmake" / "PublicMobileTitle.cmake"


class PublicTitleGateTests(unittest.TestCase):
    def check_gate(self, *, local_only=False, extra=None, wrong_digest=False):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            inputs = root / "input"
            inputs.mkdir()
            source = b"int synthetic_fixture() { return 0; }\n"
            (inputs / "fixture.cpp").write_bytes(source)
            draft = {
                "local_only": local_only,
                "files": {"fixture.cpp": hashlib.sha256(source).hexdigest()},
            }
            manifest = json.dumps(draft).encode()
            (inputs / "TITLE_SOURCE_MANIFEST.json").write_bytes(manifest)
            digest = hashlib.sha256(manifest).hexdigest()
            if extra:
                (inputs / extra).write_bytes(b"synthetic fixture")
            if wrong_digest:
                digest = "0" * 64
            script = root / "check.cmake"
            script.write_text(
                f'include("{GATE}")\nremasked_check_public_title("{inputs}" "{digest}")\n'
            )
            return subprocess.run(
                ["cmake", "-P", str(script)], capture_output=True, text=True
            )

    def test_pinned_input(self):
        result = self.check_gate()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_private_input(self):
        self.assertIn("Local-only title inputs", self.check_gate(local_only=True).stderr)

    def test_wrong_manifest(self):
        self.assertIn("digest does not match", self.check_gate(wrong_digest=True).stderr)

    def test_extra_payload(self):
        self.assertIn("Unexpected file", self.check_gate(extra="romfs.bin").stderr)

    def test_unlisted_source(self):
        self.assertIn("Unlisted file", self.check_gate(extra="other.cpp").stderr)


if __name__ == "__main__":
    unittest.main()
