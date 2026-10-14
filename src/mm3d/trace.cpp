// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include "mm3d/trace.h"
#include "mm3d/camera_live.h"
namespace MM3D::Trace {
namespace {
using Key = std::tuple<std::uint64_t, std::uint64_t, unsigned>;
struct Group {
    Uniforms previous{};
    Usage usage;
    std::array<double, 96> max_delta{};
    std::array<unsigned, 96> changes{};
    std::uint64_t frame{};
    unsigned samples{}, repeated_in_frame{}, nonfinite{};
};
std::mutex mutex;
std::map<Key, Group> groups;
std::uint64_t first_frame{}, last_frame{};
unsigned draws{}, unsupported{}, overflow{};
std::chrono::steady_clock::time_point start_time;
QString reason = "Idle";
}
void Start() {
    std::scoped_lock lock{mutex};
    groups.clear(); draws = unsupported = overflow = 0;
    first_frame = last_frame = 0;
    start_time = std::chrono::steady_clock::now();
    reason = "Capturing";
    active.store(true);
}
void Stop() {
    std::scoped_lock lock{mutex};
    active.store(false); reason = "Stopped";
}
void Observe(std::uint64_t frame, std::uint64_t shader, std::uint64_t swizzle,
             unsigned vertices, bool accelerated, const Uniforms& uniforms, std::span<const std::uint32_t> code) {
    if (!active.load()) return;
    std::scoped_lock lock{mutex};
    if (!active.load()) return;
    if (!draws) first_frame = frame;
    if (frame < last_frame || frame - first_frame >= 300 || draws >= 60000 ||
        std::chrono::steady_clock::now() - start_time > std::chrono::seconds(15)) {
        active.store(false); reason = "Complete (limit or session reset)"; return;
    }
    last_frame = frame;
    ++draws;
    if (!accelerated) { ++unsupported; return; }
    const Key key{shader, swizzle, vertices};
    auto it = groups.find(key);
    if (it == groups.end()) {
        if (groups.size() >= 128) { ++overflow; return; }
        it = groups.emplace(key, Group{}).first;
        if (!code.empty()) it->second.usage = AnalyzeUsage(code);
    }
    auto& group = it->second;
    if (group.samples && group.frame == frame) ++group.repeated_in_frame;
    for (unsigned slot = 0; slot < 96; ++slot) {
        bool changed = false;
        for (unsigned component = 0; component < 4; ++component) {
            const float value = uniforms[slot][component];
            const float previous = group.previous[slot][component];
            if (!std::isfinite(value)) { ++group.nonfinite; continue; }
            if (group.samples && std::isfinite(previous)) {
                const double delta = std::abs(double(value) - previous);
                group.max_delta[slot] = std::max(group.max_delta[slot], delta);
                changed |= delta > 1e-6;
            }
        }
        if (changed) ++group.changes[slot];
    }
    group.previous = uniforms; group.frame = frame; ++group.samples;
}
QString Status() {
    std::scoped_lock lock{mutex};
    if (active.load() && std::chrono::steady_clock::now() - start_time > std::chrono::seconds(15)) {
        active.store(false); reason = "Complete (time limit)";
    }
    return QString("%1 | draws %2 | groups %3 | fallback %4")
        .arg(reason).arg(draws).arg(groups.size()).arg(unsupported);
}
bool Export(const QString& path) {
    std::scoped_lock lock{mutex};
    QJsonArray signatures;
    for (const auto& [key, group] : groups) {
        const auto& [shader, swizzle, vertices] = key;
        QJsonArray changed_slots, direct_slots, dot_slots, relative_bases;
        for (unsigned i = 0; i < 96; ++i) {
            if (group.usage.direct[i]) direct_slots.append(int(i));
            if (group.usage.dot[i]) dot_slots.append(int(i));
            if (group.usage.relative_base[i]) relative_bases.append(int(i));
        }
        for (unsigned i = 0; i < 96; ++i) if (group.changes[i]) {
            changed_slots.append(QJsonObject{{"slot", int(i)}, {"changes", int(group.changes[i])},
                {"max_component_delta", group.max_delta[i]}});
        }
        signatures.append(QJsonObject{{"shader_hash", QString::number(shader, 16)},
            {"swizzle_hash", QString::number(swizzle, 16)}, {"vertices", int(vertices)},
            {"samples", int(group.samples)}, {"repeated_in_frame", int(group.repeated_in_frame)},
            {"nonfinite_components", int(group.nonfinite)}, {"changing_slots", changed_slots},
            {"usage_known", group.usage.known}, {"statically_referenced_slots", direct_slots},
            {"direct_dot_product_slots", dot_slots}, {"relative_uniform_bases", relative_bases}});
    }
    const auto bytes = QJsonDocument(QJsonObject{{"schema", 1},
        {"kind", "read-only uniform change census; signatures are not actor identities"},
        {"first_guest_gsp_frame", double(first_frame)}, {"last_guest_gsp_frame", double(last_frame)},
        {"draws", int(draws)}, {"fallback_draws", int(unsupported)},
        {"group_overflow_draws", int(overflow)}, {"active", active.load()},
        {"groups", signatures}}).toJson();
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
}

// Bounded raw observations for comparing camera states. No guest values are modified.
namespace MM3D::CameraCapture {
namespace {
struct Sample {
    std::uint64_t frame, shader, swizzle;
    unsigned vertices, width, height, depth;
    Trace::Uniforms uniforms;
    DrawSource source;
    std::int64_t elapsed_ms{};
};
std::mutex capture_mutex;
std::vector<Sample> samples;
using CameraRows = std::array<std::array<float,4>,3>;
struct RenderCamera { CameraRows rows; unsigned draws{}; };
struct CameraFrame {
    std::uint64_t frame{};
    std::int64_t elapsed_ms{}, cpu_age_ms{};
    CameraLive::Snapshot cpu;
    std::vector<RenderCamera> render;
    unsigned candidate_overflow{};
};
std::vector<CameraFrame> camera_frames;
bool interpolation_mode{}, draws_truncated{};
void Finish() {
    active.store(false);
    CameraLive::capture_requested.store(false);
}

struct UploadWord {
    std::uint64_t frame, shader;
    std::uint32_t list_address, word_address, raw_word;
    unsigned slot;
    bool float32, completed;
    std::array<float,4> decoded;
};
std::vector<UploadWord> uploads;
bool uploads_truncated{};
std::chrono::steady_clock::time_point ready_time;
std::uint64_t first_frame{}, last_frame{};
unsigned fallback{}, capture_frame_limit{10};
bool seen_frame{};
QString reason = "Idle";
void Poll() {
    if(active.load() && std::chrono::steady_clock::now() > ready_time + std::chrono::seconds(interpolation_mode ? 20 : 10)) {
        Finish(); reason = "Complete (time limit)";
    }
}
}
void Start(unsigned delay_seconds, bool trace_uploads, unsigned frame_limit) {
    std::scoped_lock lock{capture_mutex};
    samples.clear(); samples.reserve(4096);
    camera_frames.clear(); interpolation_mode=false; draws_truncated=false;
    CameraLive::capture_requested.store(false);
    capture_frame_limit = std::clamp(frame_limit, 1u, 60u);
    uploads.clear(); uploads_truncated = false; upload_mode.store(trace_uploads);
    fallback = 0; first_frame = last_frame = 0; seen_frame = false;
    ready_time = std::chrono::steady_clock::now() + std::chrono::seconds(delay_seconds);
    reason = "Capturing"; active.store(true);
}
void StartInterpolation(unsigned delay_seconds) {
    Start(delay_seconds, false, 60);
    std::scoped_lock lock{capture_mutex};
    interpolation_mode=true;
    CameraLive::capture_requested.store(true);
}
void Stop() {
    std::scoped_lock lock{capture_mutex};
    Finish(); reason = "Stopped";
}
QString Status() {
    std::scoped_lock lock{capture_mutex};
    Poll();
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        ready_time - std::chrono::steady_clock::now()).count();
    if(active.load() && remaining > 0)
        return QString("Capture starts in %1 s — close F1 and return to the game").arg((remaining+999)/1000);
    if(interpolation_mode) {
        const auto elapsed=std::max<std::int64_t>(0,-remaining);
        return QString("%1 | %2 / 20 s | %3 camera frames | %4 sampled draws%5")
            .arg(reason).arg(elapsed/1000).arg(camera_frames.size()).arg(samples.size())
            .arg(draws_truncated ? " (draw sample cap reached; camera recording continues)" : "");
    }
    return QString("%1 | %2 draws | %3 software draws skipped | %4 upload words%5")
        .arg(reason).arg(samples.size()).arg(fallback).arg(uploads.size())
        .arg(uploads_truncated ? " (truncated)" : "");
}
void Observe(std::uint64_t frame, std::uint64_t shader, std::uint64_t swizzle,
             unsigned vertices, bool accelerated, unsigned width, unsigned height,
             unsigned depth_state, const Trace::Uniforms& uniforms, DrawSource source) {
    if(!active.load()) return;
    std::scoped_lock lock{capture_mutex};
    Poll();
    if(!active.load() || std::chrono::steady_clock::now() < ready_time) return;
    if(!seen_frame) { first_frame = frame; seen_frame = true; }
    if(frame < last_frame || (!interpolation_mode && frame - first_frame >= capture_frame_limit)) {
        Finish(); reason = frame < last_frame ? "Stopped (session reset)" : QString("Complete (%1 guest frames)").arg(capture_frame_limit);
        return;
    }
    last_frame = frame;
    const auto now=std::chrono::steady_clock::now();
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(now-ready_time).count();
    if(interpolation_mode) {
        if(camera_frames.empty() || camera_frames.back().frame!=frame) {
            if(camera_frames.size()>=8192) { Finish(); reason="Truncated (camera frame limit)"; return; }
            const auto cpu=CameraLive::GetSnapshot();
            const auto epoch=std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            camera_frames.push_back({frame,elapsed,epoch-cpu.sampled_at_ms,cpu,{},0});
        }
        // This is a candidate filter, not a verified semantic camera identifier.
        if(accelerated && height==400 && (width==240 || width==480) &&
           uniforms[3]==std::array<float,4>{0,0,-1,0}) {
            const CameraRows rows{uniforms[70],uniforms[71],uniforms[72]};
            bool finite=true;
            for(const auto& row:rows) for(float v:row) finite &= std::isfinite(v);
            if(finite) {
                auto& candidates=camera_frames.back().render;
                auto it=std::find_if(candidates.begin(),candidates.end(),[&](const auto& c){return c.rows==rows;});
                if(it!=candidates.end()) ++it->draws;
                else if(candidates.size()<8) candidates.push_back({rows,1});
                else ++camera_frames.back().candidate_overflow;
            }
        }
    }
    if(!accelerated) { ++fallback; return; }
    // Keep all depth modes: some world effects do not test depth. Viewport is only a candidate filter.
    if(height != 400 || (width != 240 && width != 480)) return;
    // Three consecutive frames per 30-frame block retain adjacent pairs for the
    // existing matcher, without allocating every draw for a twenty-second orbit.
    if(interpolation_mode && (frame-first_frame)%30>=3) return;
    if(samples.size() >= 16384) {
        draws_truncated=true;
        if(!interpolation_mode) { Finish(); reason = "Truncated (16384 draw limit)"; }
        return;
    }
    samples.push_back({frame, shader, swizzle, vertices, width, height, depth_state, uniforms, source, elapsed});
}
void ObserveUpload(std::uint64_t frame, std::uint64_t shader, std::uint32_t list_address,
                   std::uint32_t word_address, unsigned slot, bool float32,
                   std::uint32_t raw_word, bool completed, const std::array<float,4>& decoded) {
    if(!UploadsActive() || slot >= 4) return;
    std::scoped_lock lock{capture_mutex};
    Poll();
    if(!UploadsActive() || std::chrono::steady_clock::now() < ready_time) return;
    if(!seen_frame) { first_frame = frame; seen_frame = true; }
    if(frame < last_frame || (!interpolation_mode && frame - first_frame >= capture_frame_limit)) {
        Finish(); reason = frame < last_frame ? "Stopped (session reset)" : QString("Complete (%1 guest frames)").arg(capture_frame_limit);
        return;
    }
    last_frame = frame;
    if(uploads.size() >= 16384) { uploads_truncated = true; return; }
    uploads.push_back({frame, shader, list_address, word_address, raw_word, slot, float32, completed, decoded});
}
bool Export(const QString& path, const QString& label) {
    std::scoped_lock lock{capture_mutex};
    if(active.load()) return false;
    QJsonArray draws;
    for(const auto& s : samples) {
        QJsonArray uniforms;
        for(const auto& slot : s.uniforms) {
            QJsonArray values;
            for(float value : slot) values.append(std::isfinite(value) ? QJsonValue(double(value)) : QJsonValue());
            uniforms.append(values);
        }
        draws.append(QJsonObject{{"frame", double(s.frame)}, {"shader", QString::number(s.shader,16)},
            {"swizzle", QString::number(s.swizzle,16)}, {"elapsed_ms",double(s.elapsed_ms)}, {"vertices", int(s.vertices)},
            {"width", int(s.width)}, {"height", int(s.height)}, {"depth_state", double(s.depth)},
            {"uniforms", uniforms},
            {"source", QJsonObject{{"valid",s.source.valid},
                {"vertex_base",QString::number(s.source.vertex_base,16)},
                {"first_vertex",double(s.source.first_vertex)}, {"last_vertex",double(s.source.last_vertex)},
                {"index_offset",double(s.source.index_offset)}, {"topology",double(s.source.topology)},
                {"indexed",s.source.indexed}, {"index16",s.source.index16}}}});
    }
    QJsonArray upload_words;
    for(const auto& u : uploads) {
        QJsonArray decoded;
        if(u.completed) for(float v : u.decoded)
            decoded.append(std::isfinite(v) ? QJsonValue(double(v)) : QJsonValue());
        upload_words.append(QJsonObject{{"frame",double(u.frame)},
            {"shader_at_upload",QString::number(u.shader,16)},
            {"list_physical",QString::number(u.list_address,16)},
            {"word_physical",QString::number(u.word_address,16)},
            {"raw_word",QString::number(u.raw_word,16)}, {"slot",int(u.slot)},
            {"float32",u.float32}, {"completed",u.completed}, {"decoded_xyzw",decoded}});
    }
    QJsonArray camera_json;
    const auto vec=[](const auto& values) { QJsonArray out; for(auto v:values)
        out.append(std::isfinite(v)?QJsonValue(double(v)):QJsonValue()); return out; };
    for(const auto& f:camera_frames) {
        QJsonArray render;
        for(const auto& c:f.render) {
            QJsonArray rows; for(const auto& row:c.rows) rows.append(vec(row));
            render.append(QJsonObject{{"rows",rows},{"draws",int(c.draws)}});
        }
        camera_json.append(QJsonObject{{"frame",double(f.frame)},{"elapsed_ms",double(f.elapsed_ms)},
            {"cpu",QJsonObject{{"valid",f.cpu.valid},{"sample_age_ms",double(f.cpu_age_ms)},
                {"fresh",f.cpu.sampled_at_ms!=0 && f.cpu_age_ms>=0 && f.cpu_age_ms<=250},
                {"address",QString::number(f.cpu.base,16)},{"eye",vec(f.cpu.eye)},
                {"target",vec(f.cpu.target)},{"fov",double(f.cpu.fov)}}},
            {"render_candidates",render},{"candidate_overflow",int(f.candidate_overflow)}});
    }
    const auto bytes = QJsonDocument(QJsonObject{{"schema",4}, {"label",label},
        {"requested_frames",interpolation_mode ? QJsonValue() : QJsonValue(int(capture_frame_limit))},
        {"capture_seconds",interpolation_mode ? 20 : 10},{"interpolation_mode",interpolation_mode},
        {"draws_truncated",draws_truncated},{"camera_frames",camera_json},
        {"timing_note","Host steady-clock milliseconds since countdown ended; CPU readings are asynchronous, check sample_age_ms and fresh."},
        {"projection_uploads_enabled",upload_mode.load()},
        {"projection_upload_words",upload_words}, {"uploads_truncated",uploads_truncated},
        {"address_kind","guest physical GPU command-buffer words; not CPU PCs or camera object addresses"},
        {"kind","raw vertex uniforms before host projection correction; camera identity unverified"},
        {"stop_reason",reason}, {"fallback_draws",int(fallback)}, {"first_frame",double(first_frame)},
        {"last_frame",double(last_frame)}, {"draws",draws}}).toJson(QJsonDocument::Compact);
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size() && file.commit();
}
}
