# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('apple_scenes',Path(__file__).parents[1]/'tools/patch_sdl_scenes.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class ScenePatchTests(unittest.TestCase):
    def test_scene_lifecycle_is_installed_once(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);ui=root/'src/video/uikit';ui.mkdir(parents=True)
            delegate=ui/'SDL_uikitappdelegate.m';window=ui/'SDL_uikitwindow.m'
            delegate.write_text('- (void)postFinishLaunch\n{\n    /* Hide the launch screen the next time the run loop is run. */\n    SDL_iPhoneSetEventPump(SDL_FALSE);\n}\n@implementation SDLUIKitDelegate {\n    [self performSelector:@selector(postFinishLaunch) withObject:nil afterDelay:0.0];\n@end\n')
            window.write_text('        uiwindow = [[SDL_uikitwindow alloc] initWithFrame:data.uiscreen.bounds];\n')
            events=ui/'SDL_uikitevents.m'
            events.write_text('    /* See the comment in the function definition. */\n')
            module.apply(root);first=(delegate.read_text(),window.read_text(),events.read_text());module.apply(root)
            self.assertEqual(first,(delegate.read_text(),window.read_text(),events.read_text()))
            self.assertEqual(first[0].count('@interface ReMaskedSceneDelegate'),1)
            for name in ['WillResignActive','DidEnterBackground','WillEnterForeground','DidBecomeActive']:
                self.assertEqual(first[0].count('SDL_OnApplication'+name+'();'),1)

            self.assertIn('applicationState != UIApplicationStateActive',first[0])
            self.assertIn('CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, TRUE)',first[2])

            self.assertEqual(first[0].count('ReMaskedIOSDeferMain(ReMaskedResumeMain)'),1)
            self.assertEqual(first[0].count('ReMaskedIOSDidFinishGame();'),1)
