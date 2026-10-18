"""Synthetic bounds and failure tests; contains no game assets."""
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).parent/'mod_import'))
import binary
import generate

class ImportTests(unittest.TestCase):
    def test_literal_archive_round_trip_and_truncation(self):
        original=bytes(range(256))*17
        encoded=binary.literals(original,b'LzS\x01'+bytes(4))
        self.assertEqual(binary.decompress(encoded),original)
        for length in (0,4,15,len(encoded)-1):
            with self.assertRaises(ValueError):binary.decompress(encoded[:length])

    def test_oversized_decompression_is_rejected(self):
        encoded=b'LzS\x01'+bytes(4)+struct.pack('<II',64*1024*1024+1,0)
        with self.assertRaises(ValueError):binary.decompress(encoded)

    def test_rom_reader_is_read_only_and_bounded(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'game.3ds';p.write_bytes(b'original')
            reader=binary.Reader(p)
            try:
                for start,length in ((-1,1),(0,9),(8,1),(0,-1)):
                    with self.assertRaises(ValueError):reader.read(start,length)
                with self.assertRaises(ValueError):reader.actor('actor/model')
            finally:reader.close()
            self.assertEqual(p.read_bytes(),b'original')

    def test_generation_failure_closes_first_rom_and_preserves_existing_mod(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);existing=root/'user/load/mm3d-models/dark-link/keep';existing.parent.mkdir(parents=True);existing.write_bytes(b'previous model')
            closed=[]
            class FakeReader:
                def __init__(self,path):
                    if path=='bad':raise ValueError('unreadable ROM')
                def close(self):closed.append(True)
            with patch.object(generate,'Reader',FakeReader):
                with self.assertRaisesRegex(ValueError,'unreadable ROM'):
                    generate.generate(root,'good','bad',True)
            self.assertEqual(closed,[True]);self.assertEqual(existing.read_bytes(),b'previous model')
            self.assertFalse(list(existing.parent.parent.glob('.dark-link-*')))

if __name__=='__main__':unittest.main()
