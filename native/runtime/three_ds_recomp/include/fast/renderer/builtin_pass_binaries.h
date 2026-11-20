#pragma once
// Generated from maintained renderer pass sources; no game assets or captures.
// Explicit Vulkan target per artifact, main, performance. Preserve source donor notices.
// Regenerate builtin_pass.spvpack with tools/renderer/tev_program/build_pass_artifacts.
#include "fast/renderer/pass_shader_artifact.h"

namespace Fast::Renderer {
std::span<const PassShaderArtifact> BuiltinPassArtifacts();
} // namespace Fast::Renderer
