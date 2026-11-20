// SPDX-License-Identifier: GPL-3.0-or-later
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include "oot3d/renderer/azahar_texture_pack.h"
#include "../runtime/interface_textures.h"
#include "../runtime/interface_atlas.h"
#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace Oot3d::Renderer;
int main() {
    // Native-size/UI-size pictures remain exact; only oversized classified
    // interface staging images shrink, including alpha-safe atlas borders.
    {
        uint32_t w=2,h=2;
        std::vector<uint8_t> rgba{255,0,0,255, 0,255,0,0, 0,0,255,0, 255,255,255,0};
        const auto original=rgba;
        ReMasked::LimitInterfaceAtlas(rgba,w,h,0);assert(rgba==original&&w==2&&h==2);
        ReMasked::LimitInterfaceAtlas(rgba,w,h,2);assert(rgba==original&&w==2&&h==2);
        ReMasked::LimitInterfaceAtlas(rgba,w,h,1);
        assert(w==1&&h==1&&rgba==std::vector<uint8_t>({255,0,0,64}));
        // The direct decode path has identical alpha-safe pixels without a
        // full-size staging copy, and leaves its borrowed source untouched.
        w=2;h=2;
        const auto direct=ReMasked::CopyLimitedInterfaceAtlas(original,w,h,1);
        assert(direct==rgba&&w==1&&h==1&&original[0]==255);
        w=3;h=1;rgba.assign(12,255);
        ReMasked::LimitInterfaceAtlas(rgba,w,h,2);assert(w==2&&h==1&&rgba.size()==8);
    }
    const auto root = std::filesystem::temp_directory_path() /
        ("remasked-texture-stream-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    std::ofstream(root / "pack.json") << R"({"options":{"flip_png_files":false,"use_new_hash":true}})";
    std::array<std::array<uint8_t,256>,3> native{};
    std::array<std::vector<uint8_t>,3> pixels;
    std::array<uint64_t,3> hashes{};
    for (unsigned i=0;i<3;++i) {
        native[i].fill(uint8_t(i+1)); pixels[i].assign(256,uint8_t(40+i));
        hashes[i] = AzaharCityHash64(native[i]);
        WriteAzaharTexturePng(root / MakeAzaharTextureFilename(8,8,hashes[i],0,0),8,8,pixels[i]);
    }
    {
        const auto path=root / MakeAzaharTextureFilename(8,8,hashes[0],0,0);
        const auto readBytes=[&] {std::ifstream file(path,std::ios::binary);return std::vector<char>(std::istreambuf_iterator<char>(file),{});};
        const auto before=readBytes();
        AzaharTexturePackRuntime limited;
        limited.SetInterfaceTextureHashes({hashes[0]},340,4);
        limited.Configure({.LoadCustomTextures=true,.LoadDirectory=root});limited.WaitForPendingLoads();
        auto ui=limited.ResolveAndMaybeDump({8,8,0,0,native[0],pixels[0]});
        auto scenery=limited.ResolveAndMaybeDump({8,8,0,0,native[1],pixels[1]});
        assert(ui&&ui->Width==4&&ui->Height==4&&ui->NativeHash==hashes[0]);
        assert(scenery&&scenery->Width==8&&scenery->Height==8);
        assert(before==readBytes());
    }
    AzaharTexturePackRuntime runtime;
    runtime.SetDecodedCacheBudgetBytes(680); // Two full 8x8 RGBA + mip chains.
    runtime.Configure({.LoadCustomTextures=true,.LoadDirectory=root});
    runtime.WaitForPendingLoads();
    const auto load = [&](unsigned i) {return runtime.ResolveAndMaybeDump({8,8,0,0,native[i],pixels[i]});};
    auto first=load(0), second=load(1);
    assert(first && second && *first->Rgba8==pixels[0] && *second->Rgba8==pixels[1]);
    assert(load(0)); // The first picture is now more recent than the second.
    auto third=load(2); assert(third);
    auto status=runtime.Snapshot();
    assert(status.LoadedTextures==2 && status.DecodedCacheBytes<=680 && status.DecodedCacheEvictions==1);
    assert(runtime.PollQueued(hashes[0]).State==AzaharTextureResolveState::Ready);
    assert(runtime.PollQueued(hashes[1]).State==AzaharTextureResolveState::Missing);
    assert(*second->Rgba8==pixels[1]); // An outstanding draw's data remains alive.
    // A renderer may retain the native fallback after decoding was evicted
    // before its GPU upload. Polling alone must not strand that fallback.
    auto retry=runtime.ResolveKnownOrQueue(hashes[1],runtime.Generation());
    assert(retry.State==AzaharTextureResolveState::Pending);
    runtime.WaitForPendingLoads();
    auto reloaded=runtime.PollQueued(hashes[1]).Replacement;
    assert(reloaded && *reloaded->Rgba8==pixels[1]);
    assert(runtime.ResolveKnownOrQueue(hashes[1],runtime.Generation()-1).State!=AzaharTextureResolveState::Ready);
    assert(runtime.ResolveKnownOrQueue(0xfedcba9876543210ULL,runtime.Generation()).State==AzaharTextureResolveState::Missing);
    for(unsigned i=0;i<120;++i) {assert(load(i%3)); assert(runtime.Snapshot().DecodedCacheBytes<=680);}
    runtime.SetDecodedCacheBudgetBytes(1); // Pressure clears cache ownership safely.
    assert(runtime.Snapshot().LoadedTextures==0 && *first->Rgba8==pixels[0]);
    assert(load(2)); // A picture can still be delivered under a tiny budget.
    runtime.SetDecodedCacheBudgetBytes(0); // Desktop/unlimited policy is unchanged.
    assert(load(0) && load(1));
    assert(runtime.Snapshot().LoadedTextures==3);
    // Interface texture stays sharp while newer scenery rotates through the
    // same hard limit. This is priority, not unlimited pinning.
    runtime.SetDecodedCacheBudgetBytes(1);
    runtime.SetInterfaceTextureHashes({hashes[0]}, 340);
    runtime.SetDecodedCacheBudgetBytes(680);
    assert(load(0) && load(1) && load(2));
    assert(runtime.PollQueued(hashes[0]).State==AzaharTextureResolveState::Ready);
    assert(runtime.PollQueued(hashes[1]).State==AzaharTextureResolveState::Missing);
    assert(runtime.IsInterfaceTexture(hashes[0]) && !runtime.IsInterfaceTexture(hashes[1]));
    assert(runtime.Snapshot().DecodedCacheBytes<=680);
    runtime.SetInterfaceTextureHashes({hashes[0],hashes[1],hashes[2]},340);
    assert(load(0) && load(1) && load(2));
    assert(runtime.Snapshot().LoadedTextures==2 && runtime.Snapshot().DecodedCacheBytes<=680);
    runtime.SetDecodedCacheBudgetBytes(16);
    assert(!runtime.IsInterfaceTexture(hashes[0]) && runtime.Snapshot().LoadedTextures==0);
    // The private manifest contributes UI only, never character textures or
    // archived backups. Explicitly supplied files remain byte-for-byte intact.
    std::ofstream(root / "owner-content.json") << R"({"files":{
      "HudOverlay/CUSTOM_HUD/tex1_512x256_0123456789ABCDEF_13_mip0.png":"fixture",
      "HenrikoMobile/UI/PAL/ENG/tex1_256x128_FEDCBA9876543210_4_mip0.png":"fixture",
      "HudOverlay/adult-link-generated/tex1_128x128_1111111111111111_12_mip0.png":"fixture",
      "HudOverlay/.backup/tex1_128x128_2222222222222222_12_mip0.png":"fixture"}})";
    const auto ui=ReMasked::InterfaceTextureHashes(root);
    assert((ui==std::vector<uint64_t>{0x0123456789ABCDEFULL,0xFEDCBA9876543210ULL}));
    std::filesystem::remove_all(root);
    std::cout << "Texture streaming: LRU, byte budget, active-reference safety, pressure trim, bounded interface priority, manifest classification and lossless reload passed\n";
}
