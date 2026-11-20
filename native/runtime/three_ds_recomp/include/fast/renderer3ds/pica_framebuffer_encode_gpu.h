#pragma once

#include <string_view>

namespace Fast::Renderer3ds {

// GPU form of EncodePicaFramebuffer. One invocation per output *word* of four
// bytes: each word is assembled here because R32_UINT is one of the storage
// image formats Vulkan guarantees, while R8_UINT storage needs
// shaderStorageImageExtendedFormats and is not portable. For each of the four
// bytes in the word the shader resolves which texel byte it is, reproduces the
// packing of the CPU oracle for that texel and selects the byte, so formats
// with two or three bytes per texel stay exact across word boundaries.
//
// Resolution contract, which the rest of the GPU path must keep: the source is
// sampled at the *internal* rendering resolution (imageWidth x imageHeight) and
// only the output is the native tiled surface (width x height). A GPU path that
// simplified this to native-resolution sampling would silently drop the internal
// render resolution.
//
// The host must validate before dispatch (the oracle refuses these, the shader
// cannot): width/height nonzero and multiples of eight, format 0-4, bytes per
// pixel consistent with the format, nonzero image extents, and
// (2 * width + 1) * imageWidth within 32 bits so the half-texel numerator cannot
// wrap.
//
// Push constants: Extent = (imageWidth, imageHeight, width, height),
//                 Layout = (format, bytesPerPixel, wordStride, totalBytes).
inline constexpr std::string_view PicaFramebufferEncodeComputeSource = R"glsl(
#version 450
layout(local_size_x = 64) in;
layout(set = 0, binding = 0) uniform sampler2D SourceImage;
layout(set = 0, binding = 1, r32ui) uniform restrict uimage2D OutputWords;
layout(push_constant) uniform Encode {
    uvec4 Extent;
    uvec4 Layout;
} encode;

// Byte `byteInTexel` of the packed texel at (x, y), matching EncodePicaFramebuffer.
uint packedTexelByte(uint x, uint y, uint byteInTexel) {
    const uint imageWidth = encode.Extent.x;
    const uint imageHeight = encode.Extent.y;
    const uint width = encode.Extent.z;
    const uint height = encode.Extent.w;
    const uint format = encode.Layout.x;
    const uint sx = ((x * 2u + 1u) * imageWidth) / (width * 2u);
    // Memory row y is the attachment's native row height - 1 - y, as in the
    // oracle: native memory stores the top window row first.
    const uint sy = (((height - 1u - y) * 2u + 1u) * imageHeight) / (height * 2u);
    // `sample` is a reserved word in GLSL: the fetched texel has its own name.
    const vec4 texel = texelFetch(SourceImage, ivec2(int(sx), int(sy)), 0);
    const uvec4 c = uvec4(texel * 255.0 + 0.5);
    if (format == 0u) {
        const uvec4 packed = uvec4(c.a, c.b, c.g, c.r);
        return packed[byteInTexel];
    }
    if (format == 1u) {
        const uvec3 packed = uvec3(c.b, c.g, c.r);
        return packed[byteInTexel];
    }
    uint packed = 0u;
    if (format == 2u) {
        packed = ((c.r >> 3u) << 11u) | ((c.g >> 2u) << 5u) | (c.b >> 3u);
    } else if (format == 3u) {
        packed = ((c.r >> 3u) << 11u) | ((c.g >> 3u) << 6u) |
                 ((c.b >> 3u) << 1u) | (c.a >> 7u);
    } else {
        packed = ((c.r >> 4u) << 12u) | ((c.g >> 4u) << 8u) |
                 ((c.b >> 4u) << 4u) | (c.a >> 4u);
    }
    return byteInTexel == 0u ? (packed & 0xFFu) : (packed >> 8u);
}

void main() {
    const uint word = gl_GlobalInvocationID.x;
    const uint wordIndex = word * 4u;
    const uint totalBytes = encode.Layout.w;
    if (wordIndex >= totalBytes) {
        return;
    }
    const uint bytesPerPixel = encode.Layout.y;
    const uint width = encode.Extent.z;
    const uint stride = encode.Layout.z;
    uint assembled = 0u;
    for (uint i = 0u; i < 4u; ++i) {
        const uint byteIndex = wordIndex + i;
        if (byteIndex >= totalBytes) {
            break;
        }
        const uint texelIndex = byteIndex / bytesPerPixel;
        const uint byteInTexel = byteIndex % bytesPerPixel;
        // Morton order back to the native axes: three bits interleaved.
        const uint tile = texelIndex / 64u;
        const uint morton = texelIndex % 64u;
        const uint x = (tile % (width / 8u)) * 8u +
                       ((morton & 1u) | ((morton >> 1u) & 2u) | ((morton >> 2u) & 4u));
        const uint y = (tile / (width / 8u)) * 8u +
                       (((morton >> 1u) & 1u) | ((morton >> 2u) & 2u) | ((morton >> 3u) & 4u));
        assembled |= packedTexelByte(x, y, byteInTexel) << (i * 8u);
    }
    imageStore(OutputWords,
               ivec2(int((wordIndex / 4u) % stride), int((wordIndex / 4u) / stride)),
               uvec4(assembled, 0u, 0u, 0u));
}
)glsl";

} // namespace Fast::Renderer3ds
