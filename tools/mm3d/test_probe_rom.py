import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from probe_rom import FINGERPRINT_FIELDS, FINGERPRINT_FILE, identify, probe

class ProbeTest(unittest.TestCase):
    def test_regional_reference_is_identified_only_by_exact_fingerprint(self):
        manifest = json.loads(FINGERPRINT_FILE.read_text())
        self.assertEqual(identify(manifest, manifest), {'region': 'USA', 'status': 'supported'})
        [europe] = [item for item in manifest['regional_candidates'] if item['region'] == 'EUR']
        self.assertEqual(identify(europe, manifest), {'region': 'EUR', 'status': 'reference-only'})
        for field in FINGERPRINT_FIELDS:
            with self.subTest(field=field):
                self.assertIsNone(identify({**europe, field: 'different'}, manifest))

    def test_synthetic_container_and_rejections(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'fixture.cci'
            data = bytearray(2048)
            data[0x100:0x104] = b'NCSD'
            struct.pack_into('<II', data, 0x120, 1, 3)
            data[0x300:0x304] = b'NCCH'
            data[0x38f] = 4
            struct.pack_into('<Q', data, 0x318, 123)
            struct.pack_into('<H', data, 0x40e, 7)
            path.write_bytes(data)
            result = probe(path)
            self.assertEqual(result['program_id'], '000000000000007B')
            self.assertEqual(result['remaster_version'], 7)
            self.assertEqual(result['main_partition_sha256'], hashlib.sha256(data[512:]).hexdigest())
            self.assertFalse(result['revision_verified'])
            self.assertEqual(path.read_bytes(), data)
            data[0x38f] = 0
            path.write_bytes(data)
            with self.assertRaisesRegex(ValueError, 'encrypted'):
                probe(path)
            path.write_bytes(data[:400])
            with self.assertRaises(ValueError):
                probe(path)
            path.write_bytes(bytes(2048))
            with self.assertRaisesRegex(ValueError, 'NCSD'):
                probe(path)

if __name__ == '__main__':
    unittest.main()
