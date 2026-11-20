#pragma once

#include "fast/renderer3ds/pica_shader_hooks.h"

#include <string>

namespace Fast::Renderer3ds {

struct PicaVertexSurfaceShader {
    std::string Declarations;
    std::string MainBody;
    bool Available = false;
};

// Consumes only the structurally recovered native program. The native vertex
// invocation still owns position, UVs, alpha, and all skinning control flow.
[[nodiscard]] PicaVertexSurfaceShader BuildPicaVertexSurfaceShader(
    const PicaVertexShaderHookLayout& hooks);

} // namespace Fast::Renderer3ds
