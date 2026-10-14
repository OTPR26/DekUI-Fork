// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <mutex>
#include <vector>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include "mm3d/camera_writer.h"
namespace MM3D::CameraWriter {
namespace {
std::mutex mutex;
std::vector<Hit> hits;
std::chrono::steady_clock::time_point ready, deadline;
QString reason = "Idle";
unsigned duplicates{};
void Poll() {
    if(active.load() && std::chrono::steady_clock::now() >= deadline) {
        active.store(false); reason = "Complete (10-second limit)";
    }
}
}
void Start(unsigned delay_seconds) {
    std::scoped_lock lock{mutex};
    hits.clear(); duplicates = 0;
    ready = std::chrono::steady_clock::now() + std::chrono::seconds(delay_seconds);
    deadline = ready + std::chrono::seconds(10);
    reason = "Tracing projection coefficient writes";
    active.store(true);
}
void Stop() {
    std::scoped_lock lock{mutex};
    active.store(false); reason = "Stopped";
}
bool ShouldTrace() {
    if(!active.load()) return false;
    std::scoped_lock lock{mutex}; Poll();
    return active.load() && std::chrono::steady_clock::now() >= ready;
}
void Observe(const Hit& hit) {
    if(!active.load() || !Matches(hit.value)) return;
    std::scoped_lock lock{mutex}; Poll();
    if(!active.load() || std::chrono::steady_clock::now() < ready) return;
    for(const auto& old : hits) {
        if(old.process == hit.process && old.instruction.pc == hit.instruction.pc &&
           old.instruction.lr == hit.instruction.lr && old.address == hit.address && old.value == hit.value) {
            ++duplicates; return;
        }
    }
    hits.push_back(hit);
    if(hits.size() >= 128) { active.store(false); reason = "Complete (128 unique writes)"; }
}
QString Status() {
    std::scoped_lock lock{mutex}; Poll();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(ready - std::chrono::steady_clock::now()).count();
    if(active.load() && ms > 0) return QString("CPU trace starts in %1 s — close F1").arg((ms+999)/1000);
    return QString("%1 | %2 unique writes | %3 repeated writes").arg(reason).arg(hits.size()).arg(duplicates);
}
bool Export(const QString& path) {
    std::scoped_lock lock{mutex};
    if(active.load()) return false;
    QJsonArray records;
    for(const auto& h : hits) {
        QJsonArray stack;
        for(unsigned i=0;i<h.stack.size();++i)
            stack.append((h.stack_valid & (1u << i)) ? QJsonValue(QString::number(h.stack[i],16)) : QJsonValue());
        records.append(QJsonObject{{"pc",QString::number(h.instruction.pc,16)},
            {"lr",QString::number(h.instruction.lr,16)}, {"sp",QString::number(h.instruction.sp,16)},
            {"thumb",h.instruction.thumb}, {"instruction_word",QString::number(h.instruction_word,16)},
            {"destination_virtual",QString::number(h.address,16)}, {"value_bits",QString::number(h.value,16)},
            {"value",double(std::bit_cast<float>(h.value))}, {"core",int(h.core)},
            {"process",int(h.process)}, {"stack_words",stack}});
    }
    const auto bytes = QJsonDocument(QJsonObject{{"schema",1}, {"kind","CPU writers of 45-degree projection coefficients; candidates, not verified camera setters"},
        {"address_kind","guest virtual addresses; PC/LR/SP sampled before executing the write instruction"},
        {"coverage","Dynarmic 32/64-bit CPU stores and successful exclusive stores; excludes byte/halfword assembly and HLE memory copies"},
        {"stop_reason",reason}, {"duplicate_writes",int(duplicates)}, {"hits",records}}).toJson();
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size() && file.commit();
}
}
