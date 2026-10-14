// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/effect_capture.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <cmath>
namespace MM3D::EffectCapture {
bool Export(const std::string& path) {
    std::vector<Draw> samples;bool capped,geometry_capped;
    {std::scoped_lock lock(mutex);samples.swap(draws);capped=truncated;geometry_capped=geometry_truncated;}
    QJsonArray output;
    for(const auto& d:samples) {
        QJsonArray uniforms,textures,state,buffers,formats,cpu_inputs,cpu_order,cpu_input_registers;
        for(auto reg:d.cpu_input_registers)cpu_input_registers.append(int(reg));
        for(const auto& input:d.cpu_inputs) {
            QJsonArray attributes;
            for(const auto& attribute:input) {
                QJsonArray values;
                for(float v:attribute)values.append(std::isfinite(v)?QJsonValue(double(v)):QJsonValue());
                attributes.append(values);
            }
            cpu_inputs.append(attributes);
        }
        for(auto index:d.cpu_order)cpu_order.append(double(index));
        for(const auto& f:d.formats)formats.append(QJsonArray{int(f[0]),int(f[1]),int(f[2])});
        for(const auto& b:d.buffers) {
            QJsonArray components;for(auto c:b.components)components.append(int(c));
            buffers.append(QJsonObject{{"stride",int(b.stride)},{"first",int(b.first)},{"last",int(b.last)},
                {"components",components},{"hex",QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(b.bytes.data()),int(b.bytes.size())).toHex())}});
        }
        for(const auto& row:d.uniforms) {
            QJsonArray r;for(float v:row)r.append(std::isfinite(v)?QJsonValue(double(v)):QJsonValue());
            uniforms.append(r);
        }
        for(auto t:d.textures)textures.append(QString::number(t,16));
        for(auto v:d.state)state.append(double(v));
        output.append(QJsonObject{{"frame",double(d.frame)},{"ms",double(d.time)},
            {"shader",QString::number(d.shader,16)},{"swizzle",QString::number(d.swizzle,16)},
            {"vertices",double(d.vertices)},{"vertex_base",QString::number(d.base,16)},
            {"index_offset",double(d.offset)},{"indexed",d.indexed},{"accelerated",d.accelerated},
            {"textures",textures},{"state",state},{"uniforms",uniforms},{"formats",formats},{"buffers",buffers},
            {"cpu_inputs",cpu_inputs},{"cpu_order",cpu_order},
            {"cpu_input_registers",cpu_input_registers},{"cpu_topology",int(d.cpu_topology)}});
    }
    const auto data=QJsonDocument(QJsonObject{{"schema",3},{"kind","effect-render-capture"},
        {"truncated",capped},{"geometry_truncated",geometry_capped},{"draws",output},
        {"state_fields","blend,source,destination,equation,color_mask,depth_test,stencil_test"},
        {"sampling","Three consecutive guest frames per ten, 20 seconds after 3 second delay; top-screen native draws only."},
        {"limits","Small transparent world draws include bounded vertex buffers or resolved CPU inputs and submission order; no guest index payloads, texture pixels or fragment material uniforms. Nonfinite values are null. Local diagnostic; do not package."}}).toJson(QJsonDocument::Compact);
    QSaveFile file(QString::fromStdString(path));
    return file.open(QIODevice::WriteOnly)&&file.write(data)==data.size()&&file.commit();
}
}
