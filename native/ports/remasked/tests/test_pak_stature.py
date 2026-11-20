import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'models'))
from zobj_import.reader import stature
class StatureTests(unittest.TestCase):
    def model(self,form):
        data=bytearray(0x500c);data[0x500b]=form;return data
    def test_rig_age_fallback(self):
        for form,age in [(0,'adult'),(1,'child'),(4,'child'),(ord('h'),'child')]:
            self.assertEqual(stature('model.zobj',self.model(form),{}),age)
    def test_manifest_selected_entry_not_filename_guess(self):
        manifest={'zzplayas':{'OOT':{'adult_model':[{'file':'a.zobj'}],'child_model':[{'file':'c.zobj'}]}}}
        self.assertEqual(stature('folder/a.zobj',self.model(4),manifest),'adult')
        self.assertEqual(stature('folder/c.zobj',self.model(0),manifest),'child')
        self.assertEqual(stature('unreferenced.zobj',self.model(1),manifest),'child')
    def test_shared_game_entries_use_rig_age(self):
        manifest={'zzplayas':{'OOT':{'adult_model':'model.zobj'},'MM':{'child_model':'model.zobj'}}}
        self.assertEqual(stature('model.zobj',self.model(0),manifest),'adult')
        self.assertEqual(stature('model.zobj',self.model(4),manifest),'child')
    def test_malformed_optional_metadata_does_not_enable_height(self):
        for manifest in [None,[],{'zzplayas':[]},{'zzplayas':{'MM':False}},{'zzplayas':{'MM':{'adult_model':[None,42,{'file':42}]}}}]:
            self.assertEqual(stature('model.zobj',self.model(4),manifest),'child')
if __name__=='__main__':unittest.main()
