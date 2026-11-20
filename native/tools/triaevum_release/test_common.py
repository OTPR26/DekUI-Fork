"""Shared release-helper behavior, including the legacy Windows path budget."""

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from common import atomic_write_bytes


class AtomicWritePathCapacityTests(unittest.TestCase):
    def test_temporary_path_stays_inside_the_legacy_path_limit(self) -> None:
        """A legal destination must not be written through a longer temporary path.

        Content-addressed names inside a deep installation root already sit near
        the 260-character legacy Win32 limit, so a temporary name longer than
        the destination turns a valid write into a misleading
        "No such file or directory" failure (issue 53).
        """
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary).resolve()
            name = "a" * 32 + ".tam"
            while len(str(directory)) + 1 + len(name) < 250:
                room = 250 - len(str(directory)) - len(name) - 2
                if room <= 0:
                    break
                directory = directory / ("d" * min(room, 40))
            destination = directory / name
            self.assertLess(len(str(destination)), 260)
            observed: list[str] = []
            real_mkstemp = tempfile.mkstemp

            def recording_mkstemp(**kwargs):
                descriptor, temporary_name = real_mkstemp(**kwargs)
                observed.append(temporary_name)
                return descriptor, temporary_name

            with patch.object(tempfile, "mkstemp", recording_mkstemp):
                atomic_write_bytes(destination, b"private module")

            self.assertEqual(destination.read_bytes(), b"private module")
            self.assertEqual(len(observed), 1)
            self.assertLess(len(observed[0]), 260)


if __name__ == "__main__":
    unittest.main()
