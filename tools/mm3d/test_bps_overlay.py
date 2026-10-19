import hashlib
import json
from pathlib import Path
import struct
import unittest
import zlib
from bps_overlay import apply, number, overlay, parse


def fixture(source, target, instructions):
    patch = bytearray(b'BPS1') + number(len(source)) + number(len(target)) + number(0)
    for mode, size, arg in instructions:
        patch += number(((size-1) << 2) | mode)
        if mode == 1: patch += arg
        elif mode in (2, 3): patch += number((abs(arg) << 1) | (arg < 0))
    patch += struct.pack('<II', zlib.crc32(source), zlib.crc32(target))
    patch += struct.pack('<I', zlib.crc32(patch))
    return bytes(patch)


class BpsOverlayTests(unittest.TestCase):
    def spec(self, patch, target, edits):
        return {'baseline_patch_sha256':hashlib.sha256(patch).hexdigest(),
                'target_crc32':f'{zlib.crc32(target):08x}', 'edits':edits}

    def test_target_copy_does_not_inherit_an_earlier_overlay(self):
        source=b'ABCDEFGH'; original=source*2+b'custom'
        base=fixture(source, original, [(0,8,None),(3,8,0),(1,6,b'custom')])
        target=b'A!CDEFGH'+source+b'custom'
        patch=overlay(base,self.spec(base,target,[{'offset':1,'data':b'!'.hex()}]))
        self.assertEqual(apply(source,patch),target)
        literals=b''.join(arg for _,mode,_,arg in parse(patch)[2] if mode==1)
        self.assertNotIn(source,literals)
        self.assertEqual(len(literals),7)

    def test_overlapping_target_copy_is_preserved(self):
        source=b'ab'; original=b'ab'*12
        base=fixture(source,original,[(0,2,None),(3,22,0)])
        target=b'X'+original[1:]
        patch=overlay(base,self.spec(base,target,[{'offset':0,'data':'58'}]))
        self.assertEqual(apply(source,patch),target)
        self.assertEqual(sum(n for _,m,n,_ in parse(patch)[2] if m==1),1)

    def test_invalid_inputs_fail_before_producing_a_patch(self):
        source=b'ABCD';base=fixture(source,source,[(0,4,None)])
        for edits in [[{'offset':4,'data':'01'}], [{'offset':1,'data':'0102'},{'offset':2,'data':'03'}]]:
            with self.assertRaises(ValueError):overlay(base,self.spec(base,source,edits))
        with self.assertRaises(ValueError):apply(b'wrong',base)
        with self.assertRaises(ValueError):parse(base[:-1]+bytes([base[-1]^1]))

    def test_packaged_patch_is_reproducible_and_has_no_extra_source_literals(self):
        repo=Path(__file__).resolve().parents[2]
        root=repo/'src/android/app/src/main/assets/dekui-pc-controls'
        baseline=(repo/'resources/mm3d/project-restoration/code.bps').read_bytes()
        spec=json.loads((root/'provenance.json').read_text())
        recipe=json.loads((root/'input-edits.json').read_text())
        patch=(root/'code.bps').read_bytes()
        self.assertEqual(overlay(baseline,recipe),patch)
        self.assertEqual(hashlib.sha256(patch).hexdigest(),spec['patch_sha256'])
        authored=sum(len(bytes.fromhex(e['data'])) for e in recipe['edits'])
        self.assertEqual(authored,238)
        literals=lambda p:sum(n for _,m,n,_ in parse(p)[2] if m==1)
        self.assertLessEqual(literals(patch),literals(baseline)+authored)
        # All original-game input is represented by offsets, never new literals.
        source_size,target_size,_=parse(baseline)
        synthetic=bytes((i*13+7)%251 for i in range(source_size))
        def materialize(data):
            out=bytearray()
            for _,mode,size,arg in parse(data)[2]:
                if mode in (0,2):out.extend(synthetic[arg:arg+size])
                elif mode==1:out.extend(arg)
                else:
                    for j in range(size):out.append(out[arg+j])
            return out
        expected=materialize(baseline)
        for e in recipe['edits']:
            data=bytes.fromhex(e['data']);expected[e['offset']:e['offset']+len(data)]=data
        self.assertEqual(materialize(patch),expected)
        self.assertEqual(len(expected),target_size)
