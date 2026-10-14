import copy
import json
from pathlib import Path
import shutil
import tempfile
import unittest
import layout_migration as migration
from setup_game_folder import RESOURCES


class LayoutMigrationTests(unittest.TestCase):
    def setUp(self):
        self.policy=migration.policy(RESOURCES)

    def apply(self, obj):
        raw,held=migration.migrate((json.dumps(obj)+'\n').encode(),self.policy)
        return json.loads(raw),held

    def test_previous_defaults_upgrade_groups_preserve_hud(self):
        old=self.policy['previous'];new,held=self.apply(old)
        self.assertEqual(held,[])
        for k,v in self.policy['current'].items():
            self.assertEqual(new[k],old[k] if k in self.policy['preserve_existing'] else v,k)

    def test_reported_deck_case_preserves_all_nonlayout_preferences(self):
        old=json.loads((Path(__file__).parent/'fixtures/beta2-upgraded-deck.json').read_text())
        new,held=self.apply(old);self.assertEqual(held,[])
        prefixes=tuple(p for group in migration.GROUPS.values() for p in group)
        for k,v in old.items():
            self.assertEqual(new[k],self.policy['current'][k] if k.startswith(prefixes) and k in self.policy['current'] else v,k)
        again,held=self.apply(new);self.assertEqual(again,new)

    def test_custom_group_is_not_partially_repositioned(self):
        old=dict(self.policy['previous'],song_list_x=72.5,ocarina_prompt_x=-230,unknown_user_key='keep')
        new,held=self.apply(old);self.assertEqual(held,['songs'])
        for k,v in old.items():
            if k.startswith(migration.GROUPS['songs']) or k=='unknown_user_key':self.assertEqual(new[k],v)
        self.assertNotIn('song_banner_x',new)
        self.assertEqual(new['file_select_0_y'],self.policy['current']['file_select_0_y'])

    def test_current_customizations_not_reset(self):
        old=dict(self.policy['current'],song_panel_left_x=-50,file_select_1_x=123,item_present_1_x=77)
        new,held=self.apply(old);self.assertEqual(new,old)
        self.assertEqual(set(held),set(migration.GROUPS))

    def test_fresh_and_current_are_idempotent(self):
        obj=self.policy['current'];new,held=self.apply(obj)
        self.assertEqual(new,obj);self.assertEqual(held,[])

    def test_reject_corrupt_and_future_config(self):
        for raw in (b'{',b'[]',b'{"version":999}',b'{"version":true}',b'{"version":1,"song_list_x":NaN}'):
            with self.assertRaises(ValueError):migration.migrate(raw,self.policy)

    def test_release_gate_rejects_unreviewed_defaults_and_profiles(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);shutil.copytree(RESOURCES/'defaults',root/'defaults')
            shutil.copy2(RESOURCES/migration.RESOURCE,root/migration.RESOURCE)
            migration.policy(root)
            p=root/'defaults/presentation.json';raw=p.read_bytes()
            p.write_text(json.dumps(dict(self.policy['current'],song_list_x=999)))
            with self.assertRaises(ValueError):migration.policy(root)
            p.write_bytes(raw);(root/'defaults/rogue.json').write_text('{}')
            with self.assertRaises(ValueError):migration.policy(root)

    def test_every_changed_existing_default_has_explicit_policy(self):
        prefixes=tuple(p for group in migration.GROUPS.values() for p in group)
        for k,v in self.policy['previous'].items():
            if self.policy['current'].get(k)!=v:
                self.assertTrue(k.startswith(prefixes) or k in self.policy['preserve_existing'],k)

    def test_retired_profiles_are_explicitly_allowlisted(self):
        from upgrade_game_resources import RETIRED
        self.assertEqual(set(self.policy['retired_profiles']),set(RETIRED))
        for p in (RESOURCES/'defaults').rglob('*.json'):
            self.assertNotIn('config/mm3d-local/'+p.relative_to(RESOURCES/'defaults').as_posix(),RETIRED)
