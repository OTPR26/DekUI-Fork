import base64, hashlib, json, tempfile, unittest
from pathlib import Path
from desktop_hud import apply, apply_controls, reconstruct, ARCHIVE, CONFIG

class DesktopHudTests(unittest.TestCase):
    def fixture(self, root):
        user=root/'user';resources=root/'resources';assets=resources/'desktop-hud';assets.mkdir(parents=True)
        source=b'owned-rom-layout';target=source+b'-custom-hud'
        spec={'source_sha256':hashlib.sha256(source).hexdigest(),'sha256':hashlib.sha256(target).hexdigest(),
              'size':len(target),'ops':[{'copy':0,'size':len(source)},{'data':base64.b64encode(b'-custom-hud').decode()}],
              'presentation':{'hud_diamond_enabled':True},'previous':[{'hud_diamond_enabled':False}]}
        (assets/'layout.json').write_text(json.dumps(spec))
        play=b'owned-action-layout'
        (assets/'play.json').write_text(json.dumps({'source_sha256':hashlib.sha256(play).hexdigest(),'sha256':hashlib.sha256(play).hexdigest(),'size':len(play),'ops':[]}))
        path=user/'load/mods/0004000000125500/romfs/layout/Joker.Main.Play.gar';path.parent.mkdir(parents=True);path.write_bytes(play)
        texture=b'custom-atlas'
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

    def test_verified_rounding_variant_upgrades_and_unknown_changes_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            user,res,spec,source,target=self.fixture(Path(tmp))
            variant=b'Owned-rom-layout'
            spec['source_variants']=[{'sha256':hashlib.sha256(variant).hexdigest(),
                'normalize':[{'at':0,'before':'Tw==','after':'bw=='}]}]
            self.assertEqual(reconstruct(variant,spec),target)
            self.assertEqual(reconstruct(target,spec),target)
            with self.assertRaises(ValueError): reconstruct(variant+b'custom',spec)
            spec['source_variants'][0]['normalize'][0]['before']='WA=='
            with self.assertRaisesRegex(ValueError,'normalization'): reconstruct(variant,spec)

    def test_custom_positions_are_retained(self):
        with tempfile.TemporaryDirectory() as tmp:
            user,res,spec,source,target=self.fixture(Path(tmp));(user/CONFIG).write_text('{"hud_diamond_enabled":"custom"}')
            apply(user,res);self.assertEqual(json.loads((user/CONFIG).read_text()),{'hud_diamond_enabled':'custom'})

    def test_pc_input_patch_is_checked_backed_up_and_idempotent(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);user=root/'user';res=root/'resources';folder=res/'desktop-controls';folder.mkdir(parents=True)
            old=b'baseline-gameplay';new=b'pc-input-gameplay'
            (folder/'code.bps').write_bytes(new)
            (folder/'provenance.json').write_text(json.dumps({'patch_sha256':hashlib.sha256(new).hexdigest(),'baseline_patch_sha256':hashlib.sha256(old).hexdigest()}))
            target=user/'load/mods/0004000000125500/code.bps';target.parent.mkdir(parents=True);target.write_bytes(old)
            apply_controls(user,res);self.assertEqual(target.read_bytes(),new)
            self.assertEqual((user/'config/dekui-backups/code-before-desktop-pc-input.bps').read_bytes(),old)
            apply_controls(user,res);target.write_bytes(b'my-custom-patch')
            with self.assertRaisesRegex(ValueError,'Custom gameplay patch'):apply_controls(user,res)
            self.assertEqual(target.read_bytes(),b'my-custom-patch')

    def test_legacy_input_patch_upgrades_without_touching_custom_bindings(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); user=root/'user'; res=root/'resources'
            folder=res/'desktop-controls';folder.mkdir(parents=True)
            legacy=b'legacy-patch';new=b'source-copy-patch'
            spec={'patch_sha256':hashlib.sha256(new).hexdigest(),
                  'baseline_patch_sha256':hashlib.sha256(b'baseline').hexdigest(),
                  'legacy_patch_sha256':hashlib.sha256(legacy).hexdigest()}
            (folder/'provenance.json').write_text(json.dumps(spec));(folder/'code.bps').write_bytes(new)
            target=user/'load/mods/0004000000125500/code.bps';target.parent.mkdir(parents=True);target.write_bytes(legacy)
            controls=user/'config/qt-config.ini';controls.parent.mkdir(parents=True);controls.write_bytes(b'custom bindings')
            apply_controls(user,res);apply_controls(user,res)
            self.assertEqual(target.read_bytes(),new)
            self.assertEqual(controls.read_bytes(),b'custom bindings')

    def test_shared_android_desktop_hud_defaults_are_identical(self):
        root=Path(__file__).resolve().parents[2]
        resources=root/'resources/mm3d'
        assets=root/'src/android/app/src/main/assets'
        for name in ('layout.json','play.json'):
            self.assertEqual((resources/'desktop-hud'/name).read_bytes(),(assets/'dekui-hud8x'/name).read_bytes())
        values=json.loads((resources/'desktop-hud/layout.json').read_text())['presentation']
        for file in (resources/'defaults/presentation.json',assets/'dekui/presentation.json'):
            actual=json.loads(file.read_text())
            self.assertEqual({k:actual[k] for k in values},values)
