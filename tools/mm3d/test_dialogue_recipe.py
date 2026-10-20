"""Synthetic recipe checks and a guard against restoring full dialogue tables."""
from pathlib import Path
import re
import json
import struct
import unittest
from dialogue_recipe import fnv, recipe, replace_reviewed

ROOT = Path(__file__).resolve().parents[2]
TABLES = ('single_screen_text_data.h', 'single_screen_localized_data.h',
          'classic_owl_text_data.h', 'classic_owl_return_text.h',
          'classic_double_time_text.h', 'classic_localized_text.h',
          'price_text_data.h', 'localized_price_data.h')
PATTERN = re.compile(r'\{(\d+),0x([0-9a-f]+)ULL,(\d+),0x([0-9a-f]+)ULL,\{"((?:\\[0-7]{3})*)",(\d+)\}\}')


def decode_commands(encoded, source):
    out = bytearray()
    while encoded:
        offset, count, length = struct.unpack_from('<HHH', encoded)
        encoded = encoded[6:]
        assert offset+count <= len(source) and length <= len(encoded)
        out += source[offset:offset+count] + encoded[:length]
        encoded = encoded[length:]
    return bytes(out)


class DialogueRecipes(unittest.TestCase):
    def test_reviewed_unicode_substrings_need_only_fingerprints(self):
        rule = {'length': 3, 'source_hash': f'{fnv("été".encode()):016x}'}
        self.assertEqual(replace_reviewed('été / été', rule, 'fixture'), ('fixture / fixture', 2))
        self.assertEqual(replace_reviewed('other', rule, 'fixture'), ('other', 0))
        with self.assertRaises(ValueError):
            replace_reviewed('fixture', dict(rule, length=0), '')

    def test_review_specs_do_not_retain_original_dialogue(self):
        for path in (ROOT/'resources/mm3d/text').glob('single-screen*.json'):
            spec = json.loads(path.read_text())
            for edit in spec['edits']:
                self.assertNotIn('start', edit)
                self.assertIn('token_hash', edit)
            for edit in spec.get('neutral_runs', []):
                self.assertNotIn('original', edit)
                self.assertIn('source_hash', edit)
        for name in ('double-prices', 'prices-french', 'prices-spanish'):
            spec = json.loads((ROOT/'resources/mm3d/text'/f'{name}.json').read_text())
            for group in ('rewrites', 'shorten'):
                for rules in spec.get(group, {}).values():
                    self.assertIsInstance(rules, list)
                    for rule in rules:
                        self.assertEqual(set(rule), {'source_hash', 'length', 'text'})

    def test_sparse_edits_reproduce_authored_changes(self):
        source = b'Synthetic fixture: red\x00'
        for changed in (source, b'Synthetic fixture: blue\x00', b'Fixture\x00', b''):
            match = PATTERN.fullmatch(recipe(source, changed))
            self.assertIsNotNone(match)
            encoded = bytes(int(n, 8) for n in re.findall(r'\\([0-7]{3})', match[5]))
            self.assertEqual(len(encoded), int(match[6]))
            restored = decode_commands(encoded, source) if encoded else source
            self.assertEqual(restored, changed)
            self.assertEqual(int(match[2], 16), fnv(source))
            self.assertEqual(int(match[4], 16), fnv(changed))

    def test_original_entries_have_no_embedded_fallback(self):
        count = 0
        for name in TABLES:
            data = (ROOT/'src/mm3d'/name).read_text()
            self.assertNotIn('std::string_view', data)
            matches = list(PATTERN.finditer(data))
            self.assertTrue(matches, name)
            for match in matches:
                count += 1
                encoded = bytes(int(n, 8) for n in re.findall(r'\\([0-7]{3})', match[5]))
                self.assertEqual(len(encoded), int(match[6]), name)
                if match[2] == match[4]:
                    self.assertEqual(match[1], match[3], name)
                    self.assertEqual(encoded, b'', name)
        self.assertEqual(count, 2207)

    def test_rejects_unsupported_allocations(self):
        for source, edited in ((b'', b'x'), (b'x'*4097, b'x'), (b'x', b'x'*4097)):
            with self.assertRaises(ValueError):
                recipe(source, edited)
