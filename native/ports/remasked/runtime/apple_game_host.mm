// SPDX-License-Identifier: GPL-3.0-or-later
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <TargetConditionals.h>
#include <SDL.h>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <chrono>
#import <mach/mach.h>
#import <os/proc.h>
#include "oot3d/renderer/azahar_texture_pack.h"
#include "save_sync.h"
#include "apple_memory_policy.h"
#include "apple_duo.h"
#if TARGET_OS_IOS && !TARGET_OS_TV
#include "ios/ios_host.h"
#endif
#import <CommonCrypto/CommonDigest.h>

static ReMasked::AppleMemoryPolicy appleMemoryPolicy;
static uint64_t MemoryClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool ReMaskedAppleHudAvailable() {
    NSURL* root = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"GameContent/RestorationHud/layout/Joker.Main.New.gar"];
    return [NSFileManager.defaultManager fileExistsAtPath:root.path];
}
bool ReMaskedAppleHudEnabled() {
    id value = [NSUserDefaults.standardUserDefaults objectForKey:@"ReMasked.Native.RestorationHud"];
    return value ? [value boolValue] : true;
}
void ReMaskedAppleSetHudEnabled(bool enabled) {
    [NSUserDefaults.standardUserDefaults setBool:enabled forKey:@"ReMasked.Native.RestorationHud"];
    [NSUserDefaults.standardUserDefaults synchronize];
}

void InitializeReMaskedAppleGameHost() {
    static bool initialized=false;if(initialized)return;
    @autoreleasepool {
#if TARGET_OS_IOS && !TARGET_OS_TV
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "Portrait LandscapeLeft LandscapeRight");
#endif
        // Physical tvOS permits writable game state in Caches, not Application
        // Support. Textures/game data stay in the bundle; committed saves are
        // separately protected by preferences recovery and private CloudKit.
#if TARGET_OS_TV
        constexpr NSSearchPathDirectory directory = NSCachesDirectory;
#else
        constexpr NSSearchPathDirectory directory = NSApplicationSupportDirectory;
#endif
        NSURL* support = [[NSFileManager defaultManager] URLsForDirectory:directory
            inDomains:NSUserDomainMask].firstObject;
        NSURL* root = [support URLByAppendingPathComponent:@"ReMaskedNativeGame" isDirectory:YES];
        NSError* error = nil;
        if (![[NSFileManager defaultManager] createDirectoryAtURL:root
                withIntermediateDirectories:YES attributes:nil error:&error])
            throw std::runtime_error("Cannot create the isolated native game data directory");
        NSURL* game = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"GameContent" isDirectory:YES];
        for (NSString* name in @[@"ReMaskedNativeGame.launch.json", @"presentation.json", @"runtime-presentation.json"]) {
            NSURL* destination = [root URLByAppendingPathComponent:name];
            NSURL* source = [game URLByAppendingPathComponent:name];
            if (![NSFileManager.defaultManager fileExistsAtPath:destination.path] &&
                [NSFileManager.defaultManager fileExistsAtPath:source.path] &&
                ![NSFileManager.defaultManager copyItemAtURL:source toURL:destination error:nil])
                throw std::runtime_error("Could not initialize the bundled game configuration");
        }
        NSURL* profile = [root URLByAppendingPathComponent:@"ReMaskedNativeGame.launch.json"];
        if ([NSFileManager.defaultManager fileExistsAtPath:profile.path]) {
            NSMutableDictionary* document = [[NSJSONSerialization JSONObjectWithData:
                [NSData dataWithContentsOfURL:profile] options:NSJSONReadingMutableContainers error:nil] mutableCopy];
            NSMutableArray* args = [document[@"arguments"] mutableCopy];
            if (!document || ![args isKindOfClass:NSMutableArray.class])
                throw std::runtime_error("The native launch profile is invalid; it was left unchanged");
            // Full SVC history is for short bring-up runs: retaining every
            // guest service event indefinitely grows RAM during normal play.
            [args removeObject:@"--extended-diagnostics"];
            NSString* ownedLayer = @"${runtime_dir}/GameContent/RestorationHud";
            for (NSInteger i = (NSInteger)args.count - 2; i >= 0; --i)
                if ([args[i] isEqual:@"--title-romfs-layer"] && [args[i+1] isEqual:ownedLayer]) {
                    [args removeObjectAtIndex:i+1]; [args removeObjectAtIndex:i];
                }
            if (ReMaskedAppleHudAvailable() && ReMaskedAppleHudEnabled()) {
                NSURL* hud = [game URLByAppendingPathComponent:@"RestorationHud" isDirectory:YES];
                setenv("REMASKED_GAME_HUD_LAYER", hud.fileSystemRepresentation, 1);
                setenv("REMASKED_PRESERVE_BUTTON_ART", "1", 1);
            } else {
                unsetenv("REMASKED_GAME_HUD_LAYER");
                unsetenv("REMASKED_PRESERVE_BUTTON_ART");
            }
            document[@"arguments"] = args;
            NSData* updated = [NSJSONSerialization dataWithJSONObject:document options:NSJSONWritingPrettyPrinted error:nil];
            if (!updated || ![updated writeToURL:profile options:NSDataWritingAtomic error:nil])
                throw std::runtime_error("Could not save the native HUD selection");
        }
        for (NSString* name in @[@"logs", @"logs/crashes", @"saves", @"savestates", @"textures/dump"])
            if (![NSFileManager.defaultManager createDirectoryAtURL:[root URLByAppendingPathComponent:name]
                    withIntermediateDirectories:YES attributes:nil error:nil])
                throw std::runtime_error("Could not initialize the native game data folders");
        std::filesystem::current_path(root.fileSystemRepresentation);
        setenv("REMASKED_GAME_ROOT", root.fileSystemRepresentation, 1);
        setenv("REMASKED_GAME_PRESENTATION", [[root URLByAppendingPathComponent:@"runtime-presentation.json"] fileSystemRepresentation], 1);
        NSURL* owned = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"OwnerContent" isDirectory:YES];
        if ([[NSFileManager defaultManager] fileExistsAtPath:[[owned URLByAppendingPathComponent:@"owner-content.json"] path]])
            setenv("REMASKED_BUNDLED_CONTENT", owned.fileSystemRepresentation, 1);
        SDL_SetHint(SDL_HINT_APPLE_TV_CONTROLLER_UI_EVENTS, "0");
        // The Siri Remote must never occupy player 1 ahead of a real pad.
        // SDL then delivers its navigation as keys and excludes its joystick.
        SDL_SetHint(SDL_HINT_TV_REMOTE_AS_JOYSTICK, "0");
        SDL_SetHint(SDL_HINT_APPLE_TV_REMOTE_ALLOW_ROTATION, "0");
        SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
        setenv("TRIAEVUM_NATIVE_TEXTURE_CACHE_MIB", "512", 1);
#if TARGET_OS_IOS && !TARGET_OS_TV
        // Metal's device heap is not the iOS process memory allowance. Bound
        // caches and fence-retained upload scratch independently of that heap.
        setenv("REMASKED_IOS_BOUNDED_TEXTURES", "1", 1);
        setenv("TRIAEVUM_NATIVE_TEXTURE_CACHE_MIB", "256", 1);
        setenv("TRIAEVUM_VRAM_BUDGET_MIB", "1024", 1);
        setenv("TRIAEVUM_TEXTURE_UPLOAD_ARENA_MIB", "8", 1);
#endif
        setenv("TRIAEVUM_TEXTURE_IDLE_FRAMES", "60", 1);
        setenv("TRIAEVUM_PIPELINE_PREPARE_THREADS", "1", 1);
        setenv("TRIAEVUM_STARTUP_PART_THREADS", "1", 1);
        static dispatch_source_t memoryPressure;
        memoryPressure = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
            DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL,
            dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
        dispatch_source_set_event_handler(memoryPressure, ^{
            const bool critical = (dispatch_source_get_data(memoryPressure) & DISPATCH_MEMORYPRESSURE_CRITICAL) != 0;
            appleMemoryPolicy.Warn(MemoryClockMs(), critical);
            Oot3d::Renderer::AzaharTexturePackRuntime::Instance().SetDecodedCacheBudgetBytes(
                (critical ? 16ULL : 64ULL) * 1024 * 1024);
            NSLog(@"REMASKED_MEMORY_PRESSURE critical=%d decoded cache trimmed safely", critical);
        });
        dispatch_resume(memoryPressure);
        initialized=true;
        NSLog(@"ReMasked native game host initialized in its own application sandbox");
    }
}

// Run before constructing any guest filesystem service. Recovery precedes seeding:
// app-cache eviction must never replace newer progress with the initial import.
static void ReconcileCloudBeforeGuestStarts() {
    if (!std::strlen(REMASKED_ICLOUD_CONTAINER)) return;
    ReMaskedSaveSyncTick(true);
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:8];
    while (ReMaskedSaveSyncBusy() && deadline.timeIntervalSinceNow > 0)
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    if (ReMaskedSaveSyncConflict()) {
        UIWindow* window = nil;
        for (UIScene* scene in UIApplication.sharedApplication.connectedScenes)
            if ([scene isKindOfClass:UIWindowScene.class])
                for (UIWindow* candidate in ((UIWindowScene*)scene).windows)
                    if (candidate.isKeyWindow) window = candidate;
        UIViewController* presenter = window.rootViewController;
        while (presenter.presentedViewController) presenter = presenter.presentedViewController;
        if (!presenter) throw std::runtime_error("Save conflict needs a startup selection before gameplay");
        __block bool chosen = false;
        UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"Choose your saves"
            message:@"This device and iCloud have different progress. The other copy will be backed up before changing anything."
            preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"Use iCloud saves" style:UIAlertActionStyleDefault
            handler:^(UIAlertAction*){ReMaskedSaveSyncResolve(true);chosen=true;}]];
        [alert addAction:[UIAlertAction actionWithTitle:@"Keep local saves" style:UIAlertActionStyleDefault
            handler:^(UIAlertAction*){ReMaskedSaveSyncResolve(false);chosen=true;}]];
        [presenter presentViewController:alert animated:YES completion:nil];
        while (!chosen) [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    // Only this pre-guest phase may replace saves downloaded from iCloud.
    ReMaskedSaveSyncTick(true);
    if (ReMaskedSaveSyncConflict()) throw std::runtime_error("Save conflict was not resolved; progress is unchanged");
}

void InitializeReMaskedAppleSaves(const char* directory) {
    @autoreleasepool {
        NSFileManager* fm = NSFileManager.defaultManager;
        NSString* saves = [NSString stringWithUTF8String:directory];
        if (![fm createDirectoryAtPath:saves withIntermediateDirectories:YES attributes:nil error:nil] ||
            !ReMaskedSaveSyncRecoverLocal(directory))
            throw std::runtime_error("Save recovery failed; existing progress was left unchanged");
        NSString* marker = @"ReMaskedTV.OwnerSaveImported.v1";
        const char* content = std::getenv("REMASKED_BUNDLED_CONTENT");
        if (content && ![NSUserDefaults.standardUserDefaults boolForKey:marker]) {
            NSString* owned = [NSString stringWithUTF8String:content];
            NSDictionary* manifest = [NSJSONSerialization JSONObjectWithData:
                [NSData dataWithContentsOfFile:[owned stringByAppendingPathComponent:@"owner-content.json"]]
                options:0 error:nil];
            NSArray* names = @[@"save0.bin", @"save1.bin", @"save2.bin"];
            bool existing = false;
            NSMutableDictionary* initial = [NSMutableDictionary dictionary];
            for (NSString* name in names) {
                existing |= [fm fileExistsAtPath:[saves stringByAppendingPathComponent:name]];
                NSData* bytes = [NSData dataWithContentsOfFile:[[owned stringByAppendingPathComponent:@"InitialSaves"] stringByAppendingPathComponent:name]];
                unsigned char digest[CC_SHA256_DIGEST_LENGTH];
                CC_SHA256(bytes.bytes, (CC_LONG)bytes.length, digest);
                NSMutableString* hash = [NSMutableString string];
                for (auto value : digest) [hash appendFormat:@"%02x", value];
                if (bytes.length != 79384 || ![hash isEqualToString:manifest[@"initial_saves"][name]])
                    throw std::runtime_error("Bundled initial save failed validation");
                initial[name] = bytes;
            }
            if (!existing) {
                // Stage a complete set before moving the directory into place.
                NSString* stage = [saves.stringByDeletingLastPathComponent stringByAppendingPathComponent:
                    [@".owner-import-" stringByAppendingString:NSUUID.UUID.UUIDString]];
                if (![fm createDirectoryAtPath:stage withIntermediateDirectories:NO attributes:nil error:nil])
                    throw std::runtime_error("Could not prepare the initial save import");
                for (NSString* name in names)
                    if (![initial[name] writeToFile:[stage stringByAppendingPathComponent:name] options:NSDataWritingAtomic error:nil])
                        throw std::runtime_error("Could not stage the initial saves");
                // Only replace an empty directory. Other files or existing progress abort.
                if ([fm contentsOfDirectoryAtPath:saves error:nil].count != 0 ||
                    ![fm removeItemAtPath:saves error:nil] || ![fm moveItemAtPath:stage toPath:saves error:nil])
                    throw std::runtime_error("Could not finish the initial save import");
            }
            ReMaskedSaveSyncConfigure(directory);
            ReMaskedSaveSyncCommitted();
            [NSUserDefaults.standardUserDefaults setBool:YES forKey:marker];
            [NSUserDefaults.standardUserDefaults synchronize];
        } else ReMaskedSaveSyncConfigure(directory);
        ReconcileCloudBeforeGuestStarts();
    }
}

// Explicit developer launch mode: verify real CloudKit readback before gameplay.
// No save deletion or replacement is performed by this verification mode.
bool RunReMaskedAppleSavePreflight() {
    const char* requested = std::getenv("REMASKED_SAVE_SYNC_PREFLIGHT");
    if (!requested || std::strcmp(requested, "1") != 0) return false;
    @autoreleasepool {
        const auto directory = std::filesystem::current_path() / "saves";
        InitializeReMaskedAppleSaves(directory.c_str());
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:40];
        while (!ReMaskedSaveSyncCloudMatchesLocal() && deadline.timeIntervalSinceNow > 0) {
            if (!ReMaskedSaveSyncBusy()) ReMaskedSaveSyncRequest();
            ReMaskedSaveSyncTick(true);
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.25]];
            if (ReMaskedSaveSyncConflict()) break;
        }
        char status[512]{}; ReMaskedSaveSyncStatus(status, sizeof(status));
        NSLog(@"REMASKED_SAVE_PREFLIGHT cloud_matches_local=%d status=%s", ReMaskedSaveSyncCloudMatchesLocal(), status);
        if (!ReMaskedSaveSyncCloudMatchesLocal()) throw std::runtime_error(status);
        return true;
    }
}

void ReMaskedAppleMemoryPulse() {
    static auto next = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now < next) return;
    next = now + std::chrono::seconds(1);
    @autoreleasepool {
        task_vm_info_data_t info{}; mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return;
        const uint64_t headroom = os_proc_available_memory();
        auto& pack = Oot3d::Renderer::AzaharTexturePackRuntime::Instance();
        constexpr bool phone = TARGET_OS_IOS && !TARGET_OS_TV;
        const uint64_t decodedMiB = appleMemoryPolicy.DecodedMiB(
            MemoryClockMs(), info.phys_footprint >> 20U, headroom >> 20U, phone);
        pack.SetDecodedCacheBudgetBytes(decodedMiB * 1024 * 1024);
        static auto nextLog = std::chrono::steady_clock::time_point{};
        if (now < nextLog) return;
        nextLog = now + std::chrono::seconds(10);
        const auto textures = pack.Snapshot();
        NSLog(@"REMASKED_MEMORY footprint_mib=%llu headroom_mib=%llu decoded_mib=%llu decoded_budget_mib=%llu evictions=%llu textures=%zu",
            info.phys_footprint >> 20U, (unsigned long long)(headroom >> 20U),
            (unsigned long long)(textures.DecodedCacheBytes >> 20U),
            (unsigned long long)(textures.DecodedCacheBudgetBytes >> 20U),
            (unsigned long long)textures.DecodedCacheEvictions, textures.LoadedTextures);
    }
}

// The C++ render loop does not return to UIKit between frames. Drain temporary
// Metal/Foundation objects each frame instead of retaining a session's worth.
bool RunReMaskedAppleFrame(const std::function<bool()>& frame) {
    @autoreleasepool {
        ReMaskedAppleDuoFrame();
#if TARGET_OS_IOS && !TARGET_OS_TV
        ReMaskedIOSFrame();
#endif
        return frame();
    }
}
