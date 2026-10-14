// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/link_pose_capture.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
namespace MM3D::LinkPoseCapture {
bool Export(const std::string& path) {
    std::scoped_lock lock{mutex};
    const auto matrices=[](const auto& list) {
        QJsonArray result;for(const auto& m:list) {
            QJsonArray a;for(float v:m)a.append(double(v));result.append(a);
        } return result;
    };
    QJsonArray ps,ds,actors;
    for(const auto& p:actor_poses) {
        QJsonArray topology;for(auto word:p.words)topology.append(double(word));
        actors.append(QJsonObject{{"frame",double(p.pose.frame)},
            {"instance",QString::number(p.pose.instance,16)},
            {"model",QString::number(p.model,16)},
            {"signature",QString::number(p.signature,16)},
            {"bones",matrices(p.pose.bones)},{"bind",matrices(p.bind)},
            {"topology",topology}});
    }
    for(const auto& p:poses)ps.append(QJsonObject{{"frame",double(p.frame)},
        {"instance",QString::number(p.instance,16)},{"output",QString::number(p.output,16)},
        {"bones",matrices(p.bones)}});
    for(const auto& d:draws)ds.append(QJsonObject{{"frame",double(d.frame)},
        {"vertices",double(d.vertices)},{"vertex_base",QString::number(d.vertex_base,16)},
        {"index_offset",double(d.index_offset)},{"palette",matrices(d.palette)}});
    const auto bytes=QJsonDocument(QJsonObject{{"schema",2},{"kind","actor-pose-correlation"},
        {"truncated",truncated},{"poses",ps},{"draws",ds},{"actors",actors},
        {"note","Read-only samples. Skeleton identity does not distinguish gameplay from menu instances. Palette slots begin at uniform 20, three rows per matrix."}}).toJson(QJsonDocument::Compact);
    QSaveFile file(QString::fromStdString(path));
    return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.commit();
}
}
