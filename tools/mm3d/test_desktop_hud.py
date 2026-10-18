import base64, hashlib, json, tempfile, unittest
from pathlib import Path
from desktop_hud import apply, reconstruct, ARCHIVE, CONFIG

class DesktopHudTests(unittest.TestCase):
    def fixture(self, root):
        user=root/'user';resources=root/'resources';assets=resources/'desktop-hud';assets.mkdir(parents=True)
        source=b'owned-rom-layout';target=source+b'-custom-hud'
        spec={'source_sha256':hashlib.sha256(source).hexdigest(),'sha256':hashlib.sha256(target).hexdigest(),
              'size':len(target),'ops':[{'copy':0,'size':len(source)},{'data':base64.b64encode(b'-custom-hud').decode()}],
              'presentation':{'hud_diamond_enabled':True},'previous':[{'hud_diamond_enabled':False}]}
        (assets/'layout.json').write_text(json.dumps(spec));texture=b'custom-atlas'
        (assets/'atlas.png').write_bytes(texture)
        (assets/'upgrade.json').write_text(json.dumps({'texture':'atlas.png','texture_sha256':hashlib.sha256(texture).hexdigest()}))
        for rel,data in ((ARCHIVE,source),(CONFIG,b'{"hud_diamond_enabled":false,"walk_speed":2}'),('save/sentinel',b'save')):
            p=user/rel;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
        return user,resources,spec,source,target

    def test_preserves_saves_preferences_and_backs_up_before_upgrade(self):
        with tempfile.TemporaryDirectory() as tmp:
            user,res,spec,source,target=self.fixture(Path(tmp));apply(user,res)
            self.assertEqual((user/ARCHIVE).read_bytes(),target)
            self.assertEqual(json.loads((user/CONFIG).read_text()),{'hud_diamond_enabled':True,'walk_speed':2})
            self.assertEqual((user/'save/sentinel').read_bytes(),b'save')
            backup=user/'config/dekui-backups';self.assertEqual(next(backup.glob('*.gar')).read_bytes(),source)
            count=len(list(backup.iterdir()));apply(user,res);self.assertEqual(len(list(backup.iterdir())),count)

    def test_rejects_damaged_recipe_and_preserves_custom_hud(self):
        with tempfile.TemporaryDirectory() as tmp:
            user,res,spec,source,target=self.fixture(Path(tmp));spec['ops'][0]['size']=999
            with self.assertRaisesRegex(ValueError,'bounds'): reconstruct(source,spec)
            (user/ARCHIVE).write_bytes(b'my-custom-layout');apply(user,res)
            self.assertEqual((user/ARCHIVE).read_bytes(),b'my-custom-layout')
            self.assertFalse((user/'load/mm3d-model-textures').exists())

    def test_custom_positions_are_retained(self):
        with tempfile.TemporaryDirectory() as tmp:
            user,res,spec,source,target=self.fixture(Path(tmp));(user/CONFIG).write_text('{"hud_diamond_enabled":"custom"}')
            apply(user,res);self.assertEqual(json.loads((user/CONFIG).read_text()),{'hud_diamond_enabled':'custom'})
