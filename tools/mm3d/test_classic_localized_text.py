import hashlib
import json
import re
import struct
import unittest
import tempfile
from pathlib import Path
import classic_localized_text as localized


class ClassicLocalizedTextTests(unittest.TestCase):
    def test_flow_and_line_breaks(self):
        for name, spec in localized.SPEC.items():
            with self.subTest(language=name):
                tokens = localized.tokens(spec)
                self.assertEqual(tokens.count((2,)), 4)
                self.assertEqual(tokens.count((0x27,)), 4)
                self.assertEqual(tokens.count((0x2f, 2)), 1)
                self.assertEqual(tokens.count((0x31, 1)), 4)
                for page in spec['pages'][:4]:
                    self.assertGreater(len(page.splitlines()), 0)
                    self.assertLessEqual(len(page.splitlines()), 4)
                # Conservative copy limit, not a substitute for font/render testing.
                for page in spec['pages'] + spec['return']:
                    self.assertLessEqual(len(page.splitlines()), 4)
                    for line in page.splitlines():
                        self.assertLessEqual(len(re.sub(r'\{[^}]+\}', '', line)), 34)
                self.assertEqual(localized.decode(localized.encode(tokens)), tokens)

    def test_bundled_native_text_and_capacity(self):
        # Synthetic native archives keep CI independent of private game files.
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp)
            for name, spec in localized.SPEC.items():
                native = localized.encode(['Native prompt', (0x2f, 2),
                    spec['choices'][0], (1,), spec['choices'][1], (0,)])
                failure = localized.encode(['No more time', (0x29, 30), (0,)])
                data = bytearray(16 + 3 * 20)
                data[:8] = b"GMSG\x01\0\0\0"
                struct.pack_into('<II', data, 8, 3, spec['language'])
                for i, (mid, raw) in enumerate(((0xc01, native), (0x1b8a, native), (0x1b94, failure))):
                    struct.pack_into('<H', data, 16+i*20, mid)
                    struct.pack_into('<IIII', data, 20+i*20, 0x3fffffff, 0xff0001, len(data), len(raw))
                    data.extend(raw)
                (source/(name+'.gmsg')).write_bytes(data)
            assets, _ = localized.build(source)
        for name, spec in localized.SPEC.items():
            data = assets[name+'.gmsg']
            self.assertEqual(struct.unpack_from('<I', data, 12)[0], spec['language'])
            entries = localized.entries(data)
            for mid, size in ((0xc01, 1536), (0x1b8a, 128)):
                raw = entries[mid][1]
                self.assertEqual(len(raw), size)
                native = localized.decode(raw)
                self.assertIn((0x2f, 2), native)
                for choice in spec['choices']:
                    self.assertIn(choice, native)
            # Last-night failure stays native and keeps its timed dismissal.
            self.assertIn((0x29, 30), localized.decode(entries[0x1b94][1]))


if __name__ == '__main__':
    unittest.main()
