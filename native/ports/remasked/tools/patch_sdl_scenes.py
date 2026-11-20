# SPDX-License-Identifier: GPL-3.0-or-later
"""Add a single-window scene lifecycle to the pinned SDL UIKit backend."""
from pathlib import Path
import sys

def change(path, before, after):
    text=path.read_text()
    if after in text:return
    if text.count(before)!=1:raise RuntimeError('SDL UIKit context changed: '+path.name)
    path.write_text(text.replace(before,after))

def apply(root):
    root=Path(root)/'src/video/uikit'
    if '@interface ReMaskedSceneDelegate' not in (root/'SDL_uikitappdelegate.m').read_text():
        change(root/'SDL_uikitappdelegate.m', '@implementation SDLUIKitDelegate {', '''/* ReMasked native: bridge SDL's main loop to the required UIKit scene lifecycle. */
@interface ReMaskedSceneDelegate : NSObject <UIWindowSceneDelegate>
@property (nonatomic, strong) UIWindow *window;
@end
@implementation ReMaskedSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options
{
    static BOOL launched = NO;
    if (!launched && [scene isKindOfClass:UIWindowScene.class]) {
        launched = YES;
        [[SDLUIKitDelegate sharedAppDelegate] performSelector:@selector(postFinishLaunch) withObject:nil afterDelay:0.0];
    }
}
@end

@implementation SDLUIKitDelegate {''')
    change(root/'SDL_uikitappdelegate.m','@implementation ReMaskedSceneDelegate', '''@implementation ReMaskedSceneDelegate
- (void)sceneWillResignActive:(UIScene *)scene { SDL_OnApplicationWillResignActive(); }
- (void)sceneDidEnterBackground:(UIScene *)scene { SDL_OnApplicationDidEnterBackground(); }
- (void)sceneWillEnterForeground:(UIScene *)scene { SDL_OnApplicationWillEnterForeground(); }
- (void)sceneDidBecomeActive:(UIScene *)scene { SDL_OnApplicationDidBecomeActive(); }''')
    change(root/'SDL_uikitappdelegate.m','    [self performSelector:@selector(postFinishLaunch) withObject:nil afterDelay:0.0];','    /* The scene delegate starts the SDL main loop after its window scene connects. */')
    # ReTuned foreground gate: UIKit can connect before GPU work is allowed,
    # or resign while a nested event pump is servicing a native settings panel.
    change(root/'SDL_uikitappdelegate.m', '- (void)postFinishLaunch\n{', '- (void)postFinishLaunch\n{\n    if (UIApplication.sharedApplication.applicationState != UIApplicationStateActive) {\n        [self performSelector:@selector(postFinishLaunch) withObject:nil afterDelay:0.05];\n        return;\n    }')
    # Keep the ordinary UIKit run loop free for Files and CloudKit until Start.
    # The optional hooks are supplied only by the iOS game target, never tvOS.
    change(root/'SDL_uikitappdelegate.m', '@implementation SDLUIKitDelegate {', '#if TARGET_OS_IOS && !TARGET_OS_TV\nextern int ReMaskedIOSDeferMain(void (*resume)(void)) __attribute__((weak_import));\nextern void ReMaskedIOSDidFinishGame(void) __attribute__((weak_import));\nstatic void ReMaskedResumeMain(void) {\n    [[SDLUIKitDelegate sharedAppDelegate] performSelector:@selector(postFinishLaunch) withObject:nil afterDelay:0.0];\n}\n#endif\n@implementation SDLUIKitDelegate {')
    if '    if (ReMaskedIOSDeferMain && ReMaskedIOSDeferMain(ReMaskedResumeMain)) return;' in (root/'SDL_uikitappdelegate.m').read_text():
        change(root/'SDL_uikitappdelegate.m', '    if (ReMaskedIOSDeferMain && ReMaskedIOSDeferMain(ReMaskedResumeMain)) return;', '    if (ReMaskedIOSDeferMain) { [self hideLaunchScreen]; if (ReMaskedIOSDeferMain(ReMaskedResumeMain)) return; }')
    change(root/'SDL_uikitappdelegate.m', '    /* Hide the launch screen the next time the run loop is run.', '#if TARGET_OS_IOS && !TARGET_OS_TV\n    if (ReMaskedIOSDeferMain) { [self hideLaunchScreen]; if (ReMaskedIOSDeferMain(ReMaskedResumeMain)) return; }\n#endif\n    /* Hide the launch screen the next time the run loop is run.')
    change(root/'SDL_uikitappdelegate.m', '    SDL_iPhoneSetEventPump(SDL_FALSE);', '    SDL_iPhoneSetEventPump(SDL_FALSE);\n#if TARGET_OS_IOS && !TARGET_OS_TV\n    if (ReMaskedIOSDidFinishGame) ReMaskedIOSDidFinishGame();\n#endif')
    change(root/'SDL_uikitevents.m', '    /* See the comment in the function definition. */', '    /* ReMasked: never return to rendering while UIKit denies foreground GPU work. */\n    while (UIKit_EventPumpEnabled && UIApplication.sharedApplication.applicationState != UIApplicationStateActive) {\n        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, TRUE);\n    }\n    /* See the comment in the function definition. */')
    change(root/'SDL_uikitwindow.m','        uiwindow = [[SDL_uikitwindow alloc] initWithFrame:data.uiscreen.bounds];','''        UIWindowScene *windowScene = nil;
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
            if ([scene isKindOfClass:UIWindowScene.class] && ((UIWindowScene *)scene).screen == data.uiscreen) {
                windowScene = (UIWindowScene *)scene;
                break;
            }
        }
        if (!windowScene) return SDL_SetError("No connected window scene for this display");
        uiwindow = [[SDL_uikitwindow alloc] initWithWindowScene:windowScene];
        uiwindow.frame = data.uiscreen.bounds;''')

if __name__=='__main__':apply(sys.argv[1])
