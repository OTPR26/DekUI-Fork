"""Verify Android's portable recipe against the public output fingerprint manifest."""
import hashlib,json,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
class RecipeTest(unittest.TestCase):
 def test_manifest_contract(self):
  recipe=json.loads((ROOT/'resources/mm3d/android-layout-recipe.json').read_text())
  manifest=json.loads((ROOT/'resources/mm3d/setup-fingerprints.json').read_text())
  self.assertEqual(len(recipe['files']),len(manifest['files']))
  for rule,entry in zip(recipe['files'],manifest['files']):
   self.assertEqual((rule['output'],rule['size'],rule['sha256']),(entry['output'],entry['size'],entry['output_sha256']))
   self.assertEqual(recipe['inputs'][rule['base']],entry['input_sha256'])
   for op in rule['ops']:
    self.assertGreaterEqual(op['at'],0)
    self.assertLess(op['at'],rule['size'])
    if 'byte' in op:self.assertIn(op['byte'],range(256))
    else:
     self.assertIn(op['copy'],recipe['inputs'])
     self.assertLessEqual(op['at']+op['length'],rule['size'])
if __name__=='__main__':unittest.main()
