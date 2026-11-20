#pragma once

#include <array>
#include <cstdint>
#include <algorithm>
#include <cmath>

namespace Fast::Renderer3ds {

struct PicaRasterCanvas {
    float X = 0;
    float Y = 0;
    float ScaleX = 1;
    float ScaleY = 1;
};

struct PicaCanvasClip {
    int32_t X = 0, Y = 0;
    uint32_t Width = 0, Height = 0;
};

inline PicaCanvasClip ClipToPicaCanvas(
    PicaCanvasClip scissor, const PicaRasterCanvas& canvas,
    uint32_t nativeWidth, uint32_t nativeHeight) noexcept {
    const auto x1 = std::max<int64_t>(scissor.X, static_cast<int64_t>(std::ceil(canvas.X)));
    const auto y1 = std::max<int64_t>(scissor.Y, static_cast<int64_t>(std::ceil(canvas.Y)));
    const auto x2 = std::min<int64_t>(int64_t(scissor.X) + scissor.Width,
        static_cast<int64_t>(std::floor(canvas.X + nativeWidth * canvas.ScaleX)));
    const auto y2 = std::min<int64_t>(int64_t(scissor.Y) + scissor.Height,
        static_cast<int64_t>(std::floor(canvas.Y + nativeHeight * canvas.ScaleY)));
    return {static_cast<int32_t>(x1), static_cast<int32_t>(y1),
            static_cast<uint32_t>(std::max<int64_t>(0, x2 - x1)),
            static_cast<uint32_t>(std::max<int64_t>(0, y2 - y1))};
}

// The target pixels outside the canvas: everything except the inner rectangle
// ClipToPicaCanvas keeps, as at most four disjoint rectangles (the rows before
// and after it across the whole width, then the columns beside it). Empty when
// the canvas covers the target.
struct PicaCanvasSurround {
    std::array<PicaCanvasClip, 4> Rects{};
    uint32_t Count = 0;
};

inline PicaCanvasSurround SurroundPicaCanvas(
    const PicaRasterCanvas& canvas, uint32_t nativeWidth, uint32_t nativeHeight,
    uint32_t targetWidth, uint32_t targetHeight) noexcept {
    PicaCanvasSurround surround;
    const auto add = [&surround](int64_t x, int64_t y, int64_t width, int64_t height) {
        if (width > 0 && height > 0) {
            surround.Rects[surround.Count++] = {static_cast<int32_t>(x), static_cast<int32_t>(y),
                                                static_cast<uint32_t>(width),
                                                static_cast<uint32_t>(height)};
        }
    };
    const auto inner = ClipToPicaCanvas({0, 0, targetWidth, targetHeight}, canvas,
                                        nativeWidth, nativeHeight);
    if (inner.Width == 0U || inner.Height == 0U) {
        add(0, 0, targetWidth, targetHeight);
        return surround;
    }
    const int64_t x0 = inner.X;
    const int64_t y0 = inner.Y;
    const int64_t x1 = x0 + inner.Width;
    const int64_t y1 = y0 + inner.Height;
    add(0, 0, targetWidth, y0);
    add(0, y1, targetWidth, int64_t{targetHeight} - y1);
    add(0, y0, x0, y1 - y0);
    add(x1, y0, int64_t{targetWidth} - x1, y1 - y0);
    return surround;
}

} // namespace Fast::Renderer3ds
