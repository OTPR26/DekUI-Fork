#pragma once

#include "fast/renderer3ds/pica_shadow_geometry.h"
#include <memory>
#include <string>
#include <vector>

namespace Fast::Renderer3ds {
// SSBO ABI: seven vec4 records per native primitive, followed by packed corner
// fan references. Native primitive order, including degenerate strips, is kept.
inline constexpr uint32_t kPicaPbrSeamTriangleWords = 7;
struct PicaPbrSeamData {
    std::vector<std::array<float, 4>> Words;
    uint32_t TriangleCount = 0;
    uint32_t SeamCount = 0;
    uint64_t Revision = 0;
    std::string Status;
};
class PicaPbrSeamCache {
  public:
    PicaPbrSeamCache();
    ~PicaPbrSeamCache();
    PicaPbrSeamCache(PicaPbrSeamCache&&) noexcept;
    PicaPbrSeamCache& operator=(PicaPbrSeamCache&&) noexcept;
    // geometryVersion must cover indices, inputs and their layouts. Zero is
    // transient. Pose/UV uniforms are separately versioned; topology is reused.
    const PicaPbrSeamData& Resolve(const PicaShadowGeometryView& geometry, const PicaVertexShaderHookLayout& hooks,
                                   uint32_t booleanUniforms, std::span<const uint8_t> floatUniforms, uint8_t coordinate,
                                   bool triangleStrip, uint64_t geometryVersion);

  private:
    struct Impl;
    std::unique_ptr<Impl> State;
};
// Shared by the material shader and the production GPU regression fixture.
std::string PicaPbrSeamShaderLibrary();
} // namespace Fast::Renderer3ds
