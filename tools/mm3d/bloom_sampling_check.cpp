// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/bloom_sampling.h"
#include <cassert>
#include <iostream>
using namespace MM3D::BloomSampling;
int main() {
    // Actual consumer shapes from effect-hide.log, including the final composite.
    for (const auto d : {
        Draw{0x89b1ab98da3d9241ULL,0x6f0b398b1fc186a9ULL,4,256,256,0,4456448,4521984,true,false},
        Draw{0x89b1ab98da3d9241ULL,0x6f0b398b1fc186a9ULL,4,32,64,0,4325376,4390912,true,false},
        Draw{0x89b1ab98da3d9241ULL,0x6f0b398b1fc186a9ULL,4,32,64,0,4390912,4456448,true,false},
        Draw{0x89b1ab98da3d9241ULL,0x6f0b398b1fc186a9ULL,4,32,64,0,4644864,4624384,true,false},
        Draw{0x89b1ab98da3d9241ULL,0x6f0b398b1fc186a9ULL,4,16,32,0,4325376,4390912,true,false},
        Draw{0x89b1ab98da3d9241ULL,0x6f0b398b1fc186a9ULL,4,16,32,0,4644864,4624384,true,false}}) {
        assert(Matches(d));
        auto bad=d; bad.shader^=1; assert(!Matches(bad));
        bad=d; bad.swizzle^=1; assert(!Matches(bad));
        bad=d; bad.vertices=72; assert(!Matches(bad));
        bad=d; bad.feedback=true; assert(!Matches(bad));
        bad=d; bad.format=12; assert(!Matches(bad));
        bad=d; bad.blending=false; assert(!Matches(bad));
        bad=d; bad.viewport_x=0; assert(!Matches(bad));
        bad=d; bad.width=512; assert(!Matches(bad));
    }
    // User also hid this forest texture. It must never enter the correction.
    assert(!Matches({0x89b1ab98da3d9241ULL,0x6f0b398b1fc186a9ULL,72,256,128,12,
                     4644864,4624384,true,false}));
    // Full-size scene copies must not be modified during the final composition.
    assert(!Matches({0x89b1ab98da3d9241ULL,0x6f0b398b1fc186a9ULL,4,256,256,0,
                     4644864,4624384,true,false}));
    std::cout << "Bloom capture signatures and scenery exclusions passed\n";
}
