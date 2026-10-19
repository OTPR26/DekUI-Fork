"""Synthetic inputs only: package bounds, rig validation and texture isolation."""

import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile
import zlib
from zobj_import.reader import unpack, select, skeleton
from zobj_import.vendor.gbi import Interp


def human():
    data = bytearray(0x6000)
    data[0x5000:0x500B] = b"MODLOADER64"
    data[0x500B] = 4
    struct.pack_into(">I", data, 0x500C, 0x06005300)
    struct.pack_into(">I", data, 0x5300, 0x06005400)
    data[0x5304] = 21
    for i in range(21):
        at = 0x5500 + i * 12
        struct.pack_into(">I", data, 0x5400 + i * 4, 0x06000000 + at)
        struct.pack_into(">hhhBBI", data, at, 1, 0, 0, i + 1 if i < 20 else 255, 255, 0)
    return data


class ZobjReaderTests(unittest.TestCase):
    def test_human_skeleton_and_cycle_rejection(self):
        d = human()
        limbs, order, slots = skeleton(d)
        self.assertEqual(len(limbs), 21)
        self.assertEqual(order, list(range(21)))
        self.assertEqual(slots, [])
        d[0x5500 + 20 * 12 + 6] = 0
        with self.assertRaises(ValueError):
            skeleton(d)

    def test_transformation_rigs_rejected(self):
        d = human()
        d[0x500B] = 3
        with self.assertRaisesRegex(ValueError, "human"):
            skeleton(d)

    def test_truncated_skeleton_rejected(self):
        d = human()
        struct.pack_into(">I", d, 0x5400 + 4 * 20, 0x06005FFF)
        with self.assertRaises(ValueError):
            skeleton(d)

    def test_zip_manifest_prefers_mm_human(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "model.pak"
            with zipfile.ZipFile(path, "w") as z:
                z.writestr(
                    "pack/package.json",
                    json.dumps(
                        {
                            "zzplayas": {
                                "MM": {"child_model": [{"file": "mm.zobj"}]},
                                "OOT": {"adult_model": "oot.zobj"},
                            }
                        }
                    ),
                )
                z.writestr("pack/mm.zobj", bytes(human()))
                z.writestr("pack/oot.zobj", b"not selected")
                z.writestr("pack/untrusted.js", "must never execute")
            name, data, _ = select(path)
            self.assertEqual(name, "pack/mm.zobj")
            self.assertEqual(data, bytes(human()))

    def test_traversal_member_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "model.pak"
            with zipfile.ZipFile(path, "w") as z:
                z.writestr("../outside.zobj", b"bad")
            with self.assertRaisesRegex(ValueError, "Unsafe"):
                unpack(path)
            self.assertFalse((Path(folder) / "outside.zobj").exists())

    def test_legacy_deflate_container(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "model.pak"
            name = b"pack/human.zobj\xff"
            compressed = zlib.compress(bytes(human()))
            data = bytearray(b"ModLoader64\0" + bytes(4))
            start = 32 + len(name)
            data.extend(
                b"DEFL" + struct.pack(">III", 32, start, start + len(compressed))
            )
            data.extend(name)
            data.extend(compressed)
            path.write_bytes(data)
            self.assertEqual(unpack(path)["pack/human.zobj"], bytes(human()))

    def test_recursive_display_list_is_bounded(self):
        d = struct.pack(">II", 0xDE000000, 0x06000000)
        self.assertEqual(Interp({6: d}).run_limb(0x06000000), [])


class ZobjVertexCacheTests(unittest.TestCase):
    def test_cached_vertices_keep_loading_limb_across_display_lists(self):
        data = bytearray(0x100)
        struct.pack_into(">IIII", data, 0, 0x01003006, 0x06000080, 0xDF000000, 0)
        struct.pack_into(">IIII", data, 0x20, 0x05000204, 0, 0xDF000000, 0)
        for i in range(3):
            struct.pack_into(
                ">hhhHhhBBBB", data, 0x80 + i * 16, i, 0, 0, 0, 0, 0, 255, 255, 255, 255
            )
        interp = Interp({6: data})
        self.assertEqual(interp.run_limb(0x06000000, 2), [])
        triangles = interp.run_limb(0x06000020, 5)
        self.assertEqual(len(triangles), 1)
        self.assertEqual([v.flex_slot for v in triangles[0].v], [2, 2, 2])


class ZobjTextureOrientationTests(unittest.TestCase):
    def test_bottom_image_row_is_stored_at_pica_v_zero(self):
        try:
            import numpy as np
            from types import SimpleNamespace
            from zobj_import.cmb_writer import Writer
        except ImportError as error:
            self.skipTest(str(error))
        writer = Writer.__new__(Writer)
        writer.native = SimpleNamespace(
            tex=[bytes(36)] * 3, materials=[bytes(0x16C)] * 3
        )
        writer.textures = []
        writer.materials = []
        writer.material_cache = {}
        writer.texdata = bytearray()
        pixels = np.zeros((8, 8, 4), dtype=np.uint8)
        pixels[0, 0] = (17, 34, 51, 255)
        pixels[7, 0] = (68, 85, 102, 255)
        writer.material(pixels, 2, 2)
        # RGBA8 is stored ABGR in PICA Morton tiles; (0,0) has Morton index zero.
        self.assertEqual(bytes(writer.texdata[:4]), bytes((255, 102, 85, 68)))


class GroundContactTests(unittest.TestCase):
    def test_floor_and_knee_constraints(self):
        try:
            from zobj_import.convert import ground_height
        except ImportError as error:
            self.skipTest(str(error))
        self.assertAlmostEqual(ground_height(250, 1000, 250, 0)[0], 0)
        self.assertEqual(ground_height(1000, 1000, 250, 0), (1000, 1.0))
        self.assertEqual(ground_height(1250, 1000, 250, 0), (1250, 1.0))
        values = [ground_height(y, 1000, 250, 0)[0] for y in (250, 500, 750, 1000)]
        self.assertEqual(values, sorted(values))
        self.assertAlmostEqual(ground_height(250, 1000, 250, 250)[0], 250)
        with self.assertRaises(ValueError):
            ground_height(250, 1000, 1000, 0)
