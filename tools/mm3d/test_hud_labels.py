import unittest
try:
    from PIL import Image, ImageChops
    from update_hud_labels import replace_labels
except ImportError:
    Image = None


@unittest.skipIf(Image is None, 'Pillow is required for HUD atlas checks')
class HudLabelTests(unittest.TestCase):
    def test_changes_only_two_label_cells_and_is_idempotent(self):
        original = Image.new('RGBA', (4096, 2048), (20, 40, 60, 255))
        updated = replace_labels(original)
        outside = updated.copy()
        outside.paste(original.crop((3840, 640, 4096, 768)), (3840, 640))
        self.assertIsNone(ImageChops.difference(original, outside).getbbox())
        self.assertEqual(replace_labels(updated).tobytes(), updated.tobytes())
        left = updated.crop((3840, 640, 3968, 768))
        right = updated.crop((3968, 640, 4096, 768))
        self.assertNotEqual(left.tobytes(), right.tobytes())
        self.assertEqual(left.getpixel((40, 100)), (255, 240, 0, 255))
        self.assertEqual(right.getpixel((64, 47))[3], 0)

    def test_rejects_wrong_atlas_dimensions(self):
        with self.assertRaises(ValueError):
            replace_labels(Image.new('RGBA', (512, 256)))
