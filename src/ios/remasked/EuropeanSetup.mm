// SPDX-License-Identifier: GPL-2.0-or-later
#import "EuropeanSetup.h"

#import <CommonCrypto/CommonDigest.h>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <stdexcept>

namespace {
void Require(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}

NSData* Read(NSURL* path) {
    NSData* data = [NSData dataWithContentsOfURL:path options:NSDataReadingMappedAlways error:nullptr];
    Require(data != nil, "Could not read a setup file.");
    return data;
}

NSDictionary* JSON(NSURL* root, NSString* name) {
    return [NSJSONSerialization JSONObjectWithData:Read([root URLByAppendingPathComponent:name])
                                           options:0 error:nullptr];
}

NSString* Hash(const void* bytes, NSUInteger size) {
    Require(size <= UINT32_MAX, "Setup file is too large.");
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes, static_cast<CC_LONG>(size), digest);
    NSMutableString* result = [NSMutableString string];
    for (auto byte : digest) [result appendFormat:@"%02x", unsigned(byte)];
    return result;
}

NSString* Hash(NSData* data) { return Hash(data.bytes, data.length); }

uint64_t Number(const uint8_t* bytes, unsigned count) {
    uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= uint64_t(bytes[i]) << (8 * i);
    return value;
}

NSDictionary* ReadLayouts(NSData* rom, NSDictionary* fingerprint, NSDictionary* inputs) {
    auto read = [&](uint64_t offset, uint64_t count) {
        Require(offset <= rom.length && count <= rom.length - offset, "Invalid cartridge bounds.");
        return static_cast<const uint8_t*>(rom.bytes) + offset;
    };
    auto word = [&](uint64_t offset) { return Number(read(offset, 4), 4); };
    Require(std::memcmp(read(0x100, 4), "NCSD", 4) == 0, "Choose a decrypted .3ds or .cci cartridge.");
    const uint64_t base = word(0x120) * 512;
    const uint64_t size = word(0x124) * 512;
    const auto* partition = read(base, size);
    Require(base >= 512 && size == [fingerprint[@"main_partition_bytes"] unsignedLongLongValue],
            "This preview requires European Rev 1.");
    Require(std::memcmp(read(base + 0x100, 4), "NCCH", 4) == 0 &&
            (read(base + 0x18f, 1)[0] & 4), "Choose a decrypted European Rev 1 cartridge.");
    Require(Number(read(base + 0x118, 8), 8) == std::strtoull([fingerprint[@"program_id"] UTF8String], nullptr, 16) &&
            Number(read(base + 0x20e, 2), 2) == [fingerprint[@"remaster_version"] unsignedIntValue] &&
            [Hash(partition, size) isEqual:fingerprint[@"main_partition_sha256"]],
            "The cartridge did not match European Rev 1.");

    const auto fsOffset = word(base + 0x1b0) * 512;
    const auto fsSize = word(base + 0x1b4) * 512;
    Require(fsSize >= 0x1000 && fsOffset <= size && fsSize <= size - fsOffset, "Invalid RomFS.");
    auto rr = [&](uint64_t offset, uint64_t count) {
        Require(offset <= fsSize - 0x1000 && count <= fsSize - 0x1000 - offset, "Invalid RomFS entry.");
        return partition + fsOffset + 0x1000 + offset;
    };
    const auto* header = rr(0, 40);
    Require(Number(header, 4) == 40, "Unsupported RomFS header.");
    const auto dirs = Number(header + 12, 4), files = Number(header + 28, 4);
    const auto payload = Number(header + 36, 4);
    auto name = [&](uint64_t at, uint64_t size) {
        Require(size <= 1024 && size % 2 == 0, "Invalid RomFS name.");
        NSString* value = [[NSString alloc] initWithBytes:rr(at, size) length:size
                                                encoding:NSUTF16LittleEndianStringEncoding];
        Require(value != nil, "Invalid RomFS name.");
        return value;
    };
    NSMutableDictionary* found = [NSMutableDictionary dictionary];
    NSMutableSet* seenDirectories = [NSMutableSet set];
    constexpr uint64_t none = UINT32_MAX;
    std::function<uint64_t(uint64_t, NSString*, unsigned)> directory =
        [&](uint64_t at, NSString* parent, unsigned depth) -> uint64_t {
            Require(depth <= 32 && ![seenDirectories containsObject:@(at)], "Invalid RomFS directory tree.");
            [seenDirectories addObject:@(at)];
            const auto* entry = rr(dirs + at, 24);
            NSString* path = [parent stringByAppendingPathComponent:name(dirs + at + 24, Number(entry + 20, 4))];
            NSMutableSet* seenFiles = [NSMutableSet set];
            for (auto file = Number(entry + 12, 4); file != none;) {
                Require(![seenFiles containsObject:@(file)], "Invalid RomFS file list.");
                [seenFiles addObject:@(file)];
                const auto* item = rr(files + file, 32);
                NSString* key = [path stringByAppendingPathComponent:name(files + file + 32, Number(item + 28, 4))];
                if (inputs[key]) {
                    auto length = Number(item + 16, 8), offset = Number(item + 8, 8);
                    Require(length <= 16 * 1024 * 1024 && offset <= fsSize && !found[key],
                            "Invalid RomFS layout.");
                    found[key] = [NSData dataWithBytes:rr(payload + offset, length) length:length];
                }
                file = Number(item + 4, 4);
            }
            for (auto child = Number(entry + 8, 4); child != none;)
                child = directory(child, path, depth + 1);
            return Number(entry + 4, 4);
        };
    directory(0, @"original/romfs", 0);
    return found;
}

void Write(NSData* data, NSURL* url) {
    Require([NSFileManager.defaultManager createDirectoryAtURL:[url URLByDeletingLastPathComponent]
                                    withIntermediateDirectories:YES attributes:nil error:nullptr] &&
            [data writeToURL:url options:NSDataWritingAtomic error:nullptr], "Could not write HUD setup files.");
}

NSData* Reconstruct(NSDictionary* item, NSDictionary* sources) {
    const auto size = [item[@"size"] unsignedIntegerValue];
    Require(size <= 16 * 1024 * 1024, "Invalid HUD output size.");
    NSMutableData* output = [sources[item[@"base"]] mutableCopy];
    Require(output != nil, "Missing HUD source.");
    output.length = size;
    auto* bytes = static_cast<uint8_t*>(output.mutableBytes);
    for (NSDictionary* op in item[@"ops"]) {
        const auto at = [op[@"at"] unsignedIntegerValue];
        if (op[@"byte"]) {
            Require(at < size, "Invalid HUD byte edit.");
            bytes[at] = [op[@"byte"] unsignedCharValue];
        } else {
            NSData* source = sources[op[@"copy"]];
            const auto offset = [op[@"offset"] unsignedIntegerValue], count = [op[@"length"] unsignedIntegerValue];
            Require(source && at <= size && count <= size - at &&
                    offset <= source.length && count <= source.length - offset, "Invalid HUD copy.");
            std::memcpy(bytes + at, static_cast<const uint8_t*>(source.bytes) + offset, count);
        }
    }
    Require([Hash(output) isEqual:item[@"sha256"]], "Reconstructed HUD failed verification.");
    return output;
}
NSData* Decode(NSString* value) {
    NSData* data = [[NSData alloc] initWithBase64EncodedString:value options:0];
    Require(data != nil, "Invalid HUD patch data.");
    return data;
}

void Replace(NSMutableData* data, NSDictionary* op) {
    const auto at = [op[@"at"] unsignedIntegerValue];
    NSData* before = Decode(op[@"before"]);
    NSData* after = Decode(op[@"after"]);
    Require(before.length == after.length && at <= data.length && after.length <= data.length - at,
            "Invalid HUD patch bounds.");
    const NSRange range = NSMakeRange(at, before.length);
    NSData* current = [data subdataWithRange:range];
    NSData* alternate = op[@"alternate"] ? Decode(op[@"alternate"]) : nil;
    Require([current isEqual:before] || [current isEqual:after] || [current isEqual:alternate],
            "HUD patch does not match the reconstructed archive.");
    [data replaceBytesInRange:range withBytes:after.bytes];
}

NSMutableData* AuthoredHUD(NSData* data, NSDictionary* recipe) {
    NSString* hash = Hash(data);
    if ([hash isEqual:recipe[@"sha256"]]) return [data mutableCopy];
    for (NSDictionary* variant in recipe[@"source_variants"]) {
        if (![hash isEqual:variant[@"sha256"]]) continue;
        NSMutableData* normalized = [data mutableCopy];
        for (NSDictionary* op in variant[@"normalize"]) Replace(normalized, op);
        data = normalized;
        break;
    }
    Require([Hash(data) isEqual:recipe[@"source_sha256"]], "The HUD archive did not match the upgrade recipe.");
    const auto size = [recipe[@"size"] unsignedIntegerValue];
    Require(size > 0 && size <= 512 * 1024, "Invalid HUD archive size.");
    NSMutableData* upgraded = [NSMutableData dataWithCapacity:size];
    for (NSDictionary* op in recipe[@"ops"]) {
        if (op[@"copy"]) {
            const auto offset = [op[@"copy"] unsignedIntegerValue], count = [op[@"size"] unsignedIntegerValue];
            Require(offset <= data.length && count <= data.length - offset, "Invalid HUD archive copy.");
            [upgraded appendBytes:static_cast<const uint8_t*>(data.bytes) + offset length:count];
        } else {
            [upgraded appendData:Decode(op[@"data"])];
        }
        Require(upgraded.length <= size, "HUD archive exceeded its expected size.");
    }
    Require(upgraded.length == size && [Hash(upgraded) isEqual:recipe[@"sha256"]], "HUD archive failed verification.");
    return upgraded;
}

void UpgradeHUD(NSURL* resources, NSURL* mods, NSURL* dataRoot) {
    NSURL* hud = [resources URLByAppendingPathComponent:@"hud"];
    NSURL* main = [mods URLByAppendingPathComponent:@"romfs/layout/Joker.Main.New.gar"];
    NSURL* play = [mods URLByAppendingPathComponent:@"romfs/layout/Joker.Main.Play.gar"];
    NSDictionary* spec = JSON(hud, @"upgrade.json");
    NSMutableData* archive = AuthoredHUD(Read(main), JSON(hud, @"layout.json"));
    Require(archive.length == [spec[@"archive_size"] unsignedIntegerValue], "HUD texture layout failed verification.");
    for (NSDictionary* op in spec[@"ops"]) Replace(archive, op);
    Write(archive, main);
    Write(AuthoredHUD(Read(play), JSON(hud, @"play.json")), play);
    NSString* name = spec[@"texture"];
    Require(name.length && [name.lastPathComponent isEqual:name], "Invalid HUD texture name.");
    NSData* texture = Read([hud URLByAppendingPathComponent:name]);
    Require([Hash(texture) isEqual:spec[@"texture_sha256"]], "HUD texture failed verification.");
    NSURL* textures = [dataRoot URLByAppendingPathComponent:@"Azahar/load/textures/0004000000125500/CUSTOM_HUD"];
    Write(texture, [textures URLByAppendingPathComponent:name]);
}
} // namespace

NSString* ImportEuropeanROM(NSURL* input, NSURL* dataRoot, NSURL* destination) {
    NSFileManager* files = NSFileManager.defaultManager;
    NSURL* stage = [dataRoot URLByAppendingPathComponent:
        [@".eur-setup-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    NSURL* stagedROM = [stage URLByAppendingPathComponent:@"game.3ds"];
    NSURL* stagedMods = [stage URLByAppendingPathComponent:@"mods"];
    NSURL* mods = [dataRoot URLByAppendingPathComponent:@"Azahar/load/mods/0004000000125500"];
    NSString* error = nil;
    bool installedMods = false;
    const BOOL access = [input startAccessingSecurityScopedResource];
    try {
        Require([files createDirectoryAtURL:stage withIntermediateDirectories:YES attributes:nil error:nullptr] &&
                [files copyItemAtURL:input toURL:stagedROM error:nullptr], "Could not copy the selected cartridge.");
        NSURL* resources = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"EuropeanSetup"];
        NSDictionary* manifest = JSON(resources, @"supported-rom.json");
        NSDictionary* fingerprint = nil;
        for (NSDictionary* entry in manifest[@"regional_candidates"])
            if ([entry[@"region"] isEqual:@"EUR"]) fingerprint = entry;
        Require(fingerprint != nil, "Missing European cartridge fingerprint.");
        NSDictionary* recipe = JSON(resources, @"android-layout-recipe.json");
        NSDictionary* inputs = recipe[@"inputs"];
        NSMutableDictionary* sources = [ReadLayouts(Read(stagedROM), fingerprint, inputs) mutableCopy];
        for (NSString* key in inputs) {
            if ([key hasPrefix:@"restoration/"]) {
                NSURL* patch = [dataRoot URLByAppendingPathComponent:key];
                Require([files fileExistsAtPath:patch.path],
                        "Add the verified Project Restoration inputs in Files → ReMasked EUR Preview → restoration.");
                sources[key] = Read(patch);
            }
            NSData* data = sources[key];
            Require(data && [Hash(data) isEqual:inputs[key]], "A HUD source file failed verification.");
        }
        NSArray* outputs = recipe[@"files"];
        Require(outputs.count > 0, "Missing HUD recipe.");
        for (NSDictionary* item in outputs) {
            NSData* bytes = Reconstruct(item, sources);
            Write(bytes, [stagedMods URLByAppendingPathComponent:item[@"output"]]);
        }
        UpgradeHUD(resources, stagedMods, dataRoot);
        if (![files fileExistsAtPath:mods.path]) {
            Require([files createDirectoryAtURL:mods.URLByDeletingLastPathComponent
                    withIntermediateDirectories:YES attributes:nil error:nullptr] &&
                    [files moveItemAtURL:stagedMods toURL:mods error:nullptr], "Could not install the ReMasked HUD.");
            installedMods = true;
        } else {
            for (NSDictionary* item in outputs)
                Require([files fileExistsAtPath:[mods URLByAppendingPathComponent:item[@"output"]].path],
                        "Existing HUD setup is incomplete.");
        }
        Require([files moveItemAtURL:stagedROM toURL:destination error:nullptr], "Could not install the verified cartridge.");
    } catch (const std::exception& failure) {
        if (installedMods) [files removeItemAtURL:mods error:nullptr];
        error = [NSString stringWithUTF8String:failure.what()];
    }
    if (access) [input stopAccessingSecurityScopedResource];
    [files removeItemAtURL:stage error:nullptr];
    return error;
}
