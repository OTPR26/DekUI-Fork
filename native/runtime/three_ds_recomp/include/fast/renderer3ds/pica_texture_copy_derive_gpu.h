#pragma once

#include <string_view>

namespace Fast::Renderer3ds {

// Materializes one region of a raw copy into the texture a native draw samples,
// from the immutable internal-resolution capture of the copy's source.
//
// One invocation per destination texel. The destination is at the declared
// native layout scaled by a per-axis density that is at least the source's own
// internal-per-native ratio, and every destination texel maps through the
// region's native translation onto a fractional position inside the capture, so
// the sampled image keeps internal-or-higher density per axis even when the
// source scale is anisotropic or non-integral (for example a native 480x400
// source rendered at 1440x1280 sampled into a declared 512x512 layout). A
// native-size destination is never produced from a scaled source.
//
// Exact integer mapping, with all coordinates in half-texel units:
//   u_source_native = (2*destination + 1) / (2*density) + nativeDelta
//   source_pixel    = floor(u_source_native * imageExtent / sourceNativeExtent)
// which reduces to the encode's own center sample
// (((2*x+1)*imageWidth)/(2*width)) when the destination is at the declared
// native layout.
//
// Rows: the destination, like native memory, stores the top window row first.
// A captured attachment holds window rows bottom-up (see
// EncodePicaFramebuffer), so for such a source the vertical native coordinate
// is mirrored first: v_capture_native = sourceNativeHeight - v_source_native.
// A source decoded from native bytes is already in memory order.
//
// The declared native format's channel semantics are applied per pixel, so a
// quantized native format is reproduced exactly: 0 RGBA8 unchanged, 1 RGB8 alpha
// 255, 2 RGB565 5/6/5, 3 RGBA5551 5/5/5/1, 4 RGBA4444 4/4/4/4, each expanded the
// way the native decoder expands it.
//
// The host must validate before dispatch: nonzero region and image extents,
// nonzero density and source native extent, format <= 4, the region inside the
// destination, and every numerator below 2^31 after the absolute native
// translation (and, for a mirrored source, 2*density*sourceNativeHeight) is
// added, so the integer arithmetic cannot wrap.
//
// Push constants: Rect   = (destinationX, destinationY, width, height),
//                 Delta  = (nativeDeltaX, nativeDeltaY, sourceAttachmentRows,
//                           0),
//                 Layout = (densityX, densityY, sourceNativeWidth,
//                           sourceNativeHeight),
//                 Extent = (imageWidth, imageHeight, nativeFormat, 0).
inline constexpr std::string_view PicaTextureCopyDeriveComputeSource = R"glsl(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0) uniform texture2D SourceImage;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D DestinationImage;
layout(set = 0, binding = 2) uniform sampler SourceSampler;
layout(push_constant) uniform Derive {
    uvec4 Rect;
    ivec4 Delta;
    uvec4 Layout;
    uvec4 Extent;
} derive;

int floorDiv(int value, int divisor) {
    const int remainder = value % divisor;
    return (remainder < 0) ? (value - remainder) / divisor - 1
                           : value / divisor;
}

void main() {
    const uvec2 local = gl_GlobalInvocationID.xy;
    if (local.x >= derive.Rect.z || local.y >= derive.Rect.w) {
        return;
    }
    const ivec2 destination = ivec2(derive.Rect.xy) + ivec2(local);
    const uint densityX = derive.Layout.x;
    const uint densityY = derive.Layout.y;
    const uint sourceNativeWidth = derive.Layout.z;
    const uint sourceNativeHeight = derive.Layout.w;
    const uint imageWidth = derive.Extent.x;
    const uint imageHeight = derive.Extent.y;
    // Fractional position inside the capture, as an exact rational.
    const int numeratorX =
        (2 * destination.x + 1 + 2 * int(densityX) * derive.Delta.x) *
        int(imageWidth);
    int rowY = 2 * destination.y + 1 + 2 * int(densityY) * derive.Delta.y;
    if (derive.Delta.z != 0) {
        // A captured attachment: window rows bottom-up.
        rowY = 2 * int(densityY) * int(sourceNativeHeight) - rowY;
    }
    const int numeratorY = rowY * int(imageHeight);
    const int denominatorX = 2 * int(densityX) * int(sourceNativeWidth);
    const int denominatorY = 2 * int(densityY) * int(sourceNativeHeight);
    const int sx = clamp(floorDiv(numeratorX, denominatorX), 0,
                         int(imageWidth) - 1);
    const int sy = clamp(floorDiv(numeratorY, denominatorY), 0,
                         int(imageHeight) - 1);
    const vec4 value = texelFetch(
        sampler2D(SourceImage, SourceSampler), ivec2(sx, sy), 0);
    const uvec4 c = uvec4(clamp(value, 0.0, 1.0) * 255.0 + 0.5);
    const uint format = derive.Extent.z;
    uvec4 outColor = c;
    if (format == 1u) {
        // RGB8 stores no alpha.
        outColor = uvec4(c.rgb, 255u);
    } else if (format == 2u) {
        // RGB565, expanded as the native decoder expands it.
        const uint r = (c.r >> 3u);
        const uint g = (c.g >> 2u);
        const uint b = (c.b >> 3u);
        outColor = uvec4((r << 3u) | (r >> 2u), (g << 2u) | (g >> 4u),
                         (b << 3u) | (b >> 2u), 255u);
    } else if (format == 3u) {
        // RGBA5551, expanded as the native decoder expands it.
        const uint r = (c.r >> 3u);
        const uint g = (c.g >> 3u);
        const uint b = (c.b >> 3u);
        outColor = uvec4((r << 3u) | (r >> 2u), (g << 3u) | (g >> 2u),
                         (b << 3u) | (b >> 2u), c.a >= 128u ? 255u : 0u);
    } else if (format == 4u) {
        // RGBA4444, expanded as the native decoder expands it.
        const uvec4 nibble = c >> 4u;
        outColor = (nibble << 4u) | nibble;
    }
    imageStore(DestinationImage, destination, vec4(outColor) / 255.0);
}
)glsl";

} // namespace Fast::Renderer3ds
