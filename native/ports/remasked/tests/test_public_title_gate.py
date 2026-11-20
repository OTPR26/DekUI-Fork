# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import subprocess
import tempfile
import unittest

GATE = Path(__file__).resolve().parents[1] / "cmake" / "PublicMobileTitle.cmake"


class PublicTitleGateTests(unittest.TestCase):
    def check_gate(self, public):
        with tempfile.TemporaryDirectory() as temp:
            script = Path(temp) / "check.cmake"
            script.write_text(
                f'include("{GATE}")\nremasked_reject_bundled_public_title()\n'
            )
            return subprocess.run(
                ["cmake", f"-DREMASKED_PUBLIC_MOBILE={public}", "-P", script],
                capture_output=True, text=True
            )

    def test_private_development_target_is_available(self):
        result = self.check_gate("OFF")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_current_static_target_cannot_be_configured_as_public(self):
        result = self.check_gate("ON")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("links a game module", result.stderr)


if __name__ == "__main__":
    unittest.main()
