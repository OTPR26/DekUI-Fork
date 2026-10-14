// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
class QString;
#include "mm3d/shader_usage.h"
namespace MM3D::Trace {
using Uniforms = std::array<std::array<float, 4>, 96>;
inline std::atomic<bool> active{false};
void Start();
void Stop();
void Observe(std::uint64_t frame, std::uint64_t shader, std::uint64_t swizzle,
             unsigned vertices, bool accelerated, const Uniforms& uniforms, std::span<const std::uint32_t> code = {});
QString Status();
bool Export(const QString& path);
}

namespace MM3D::CameraCapture {
inline std::atomic<bool> active{false};
inline std::atomic<bool> upload_mode{false};
inline bool UploadsActive() { return active.load() && upload_mode.load(); }
// Draw-source addresses are candidate matching evidence, never object identities.
struct DrawSource {
    std::uint32_t vertex_base{}, first_vertex{}, last_vertex{}, index_offset{}, topology{};
    bool indexed{}, index16{}, valid{};
};
void Start(unsigned delay_seconds = 3, bool trace_uploads = false, unsigned frame_limit = 10);
void StartInterpolation(unsigned delay_seconds = 3);
void ObserveUpload(std::uint64_t frame, std::uint64_t shader, std::uint32_t list_address,
                   std::uint32_t word_address, unsigned slot, bool float32,
                   std::uint32_t raw_word, bool completed, const std::array<float,4>& decoded);
void Stop();
QString Status();
void Observe(std::uint64_t frame, std::uint64_t shader, std::uint64_t swizzle,
             unsigned vertices, bool accelerated, unsigned width, unsigned height,
             unsigned depth_state, const Trace::Uniforms& uniforms, DrawSource source = {});
bool Export(const QString& path, const QString& label);
}
