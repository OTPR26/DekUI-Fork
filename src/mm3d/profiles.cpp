// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/profiles.h"
#include "mm3d/atlas_render.h"
#include "mm3d/texture_picker.h"
#include "mm3d/draw_probe.h"
#include "mm3d/pause_control.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <set>
#include <mutex>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
namespace MM3D::Profiles {
namespace {
std::mutex mutex;
std::string root, status;
std::map<std::string,Profile> profiles;
std::map<std::uint64_t,std::vector<Rule>> captured;
Profile preview;
Profile hidden;
bool preview_active = false;
constexpr const char* notebook_signal = "notebook/notebook_open_signal.json";
bool notebook_seen = false;
std::set<std::string> frame_contexts;
std::array<float,4> draw_uv{0,0,1,1};
bool draw_uv_valid=false;
std::map<std::string,FadeProbe> frame_fades, last_fades;
std::unordered_map<std::string, std::int64_t> context_latches;
bool Dimensions(const Rule& r, unsigned w, unsigned h, unsigned f) {
    return r.width == w && r.height == h && r.format == f;
}
void Add(std::vector<Rule>& rules, const Rule& r) {
    if (std::find(rules.begin(),rules.end(),r) == rules.end() && rules.size() < 1024) rules.push_back(r);
}
bool Valid(const Rule& r) {
    return r.width && r.height && r.width <= 2048 && r.height <= 2048 && r.format < 32 &&
        r.hash.size() == 64 && r.hash.find_first_not_of("0123456789abcdef") == std::string::npos &&
        !r.context.empty();
}
float Number(const QJsonObject& o, const char* key, float fallback, float lo, float hi) {
    const double n = o.value(key).toDouble(fallback);
    return std::isfinite(n) ? std::clamp(float(n),lo,hi) : fallback;
}
Profile Parse(const QJsonObject& o, const std::string& id) {
    Profile p; p.id=id; p.name=o["name"].toString(QFileInfo(QString::fromStdString(id)).baseName()).toStdString();
    p.enabled=o["enabled"].toBool(true);
    p.source_top=o["source_screen"].toString("bottom") == "top";
    p.required_context=o["required_context"].toString().toStdString();
    p.fade_source=o["fade_source"].toString().toStdString();
    p.fade_channel=std::clamp(o["fade_channel"].toInt(),0,6);
    p.follow_fade=o["follow_fade"].toBool(false);
    p.signal_only=o["signal_only"].toBool(false);
    p.atlas_selection=o["atlas_selection"].toBool(false) || o["atlas_grids"].isArray();
    for (const auto& value:o["atlas_grids"].toArray()) {
        const auto g=value.toObject();
        Profile::AtlasGrid grid;
        grid.hash=g["sha256"].toString().toStdString();
        grid.width=g["width"].toInt(); grid.height=g["height"].toInt();
        grid.columns=g["columns"].toInt(); grid.rows=g["rows"].toInt();
        if(grid.columns<1 || grid.columns>64 || grid.rows<1 || grid.rows>64) continue;
        for(const auto& cell:g["cells"].toArray()) {
            const int n=cell.toInt(-1);
            if(n>=0 && n<grid.columns*grid.rows && std::find(grid.cells.begin(),grid.cells.end(),n)==grid.cells.end()) grid.cells.push_back(n);
        }
        for(const auto& rectangle:g["rectangles"].toArray()) {
            const auto a=rectangle.toArray(); if(a.size()!=4) continue;
            std::array<float,4> r;
            bool valid=true;
            for(int i=0;i<4;++i) { r[i]=float(a[i].toDouble(-1)); valid &= std::isfinite(r[i]) && r[i]>=0 && r[i]<=1; }
            if(valid && r[0]<r[2] && r[1]<r[3] && grid.rectangles.size()<1024) grid.rectangles.push_back(r);
        }
        p.atlas_grids.push_back(std::move(grid));
    }
    const auto mode=o["render_mode"].toString("guest_draw");
    p.panel_copy=mode=="panel_copy";
    p.presentation=mode=="presentation_layer" || p.panel_copy;
    if (mode!="guest_draw" && !p.presentation) p.enabled=false;
    p.scale=Number(o,"scale",1,.1f,2); p.x=Number(o,"x",0,-1,1); p.y=Number(o,"y",0,-1,1);
    const auto crop=o["source_crop"].toObject();
    p.crop={Number(crop,"u0",0,0,.99f), Number(crop,"v0",0,0,.99f),
        Number(crop,"u1",1,.01f,1), Number(crop,"v1",1,.01f,1)};
    p.crop[2]=std::max(p.crop[2],p.crop[0]+.01f); p.crop[3]=std::max(p.crop[3],p.crop[1]+.01f);
    if(p.panel_copy) {
        const float px=1.f/(p.source_top?400.f:320.f), py=1.f/240.f;
        p.crop={Number(crop,"u0",0,0,1-px),Number(crop,"v0",0,0,1-py),
            Number(crop,"u1",1,px,1),Number(crop,"v1",1,py,1)};
        p.crop[2]=std::max(p.crop[2],p.crop[0]+px); p.crop[3]=std::max(p.crop[3],p.crop[1]+py);
    }
    for (const auto& key:o["keys"].toArray()) { bool ok=false; const auto k=key.toString().toULongLong(&ok,16); if(ok) p.keys.push_back(k); }
    for (const auto& value:o["content_rules"].toArray()) {
        const auto r=value.toObject();
        Rule rule{unsigned(r["width"].toInt()),unsigned(r["height"].toInt()),unsigned(r["format"].toInt()),
            r["sha256"].toString().toStdString(),r["context"].toString().toStdString()};
        if (Valid(rule)) Add(p.rules,rule);
    }
    for (const auto& value:o["excluded_draws"].toArray()) {
        const auto r=value.toObject();
        Rule rule{unsigned(r["width"].toInt()),unsigned(r["height"].toInt()),unsigned(r["format"].toInt()),
            r["sha256"].toString().toStdString(),r["context"].toString().toStdString()};
        if (Valid(rule)) Add(p.exclusions,rule);
    }
    if (p.panel_copy) { p.atlas_selection=false; p.atlas_grids.clear(); }
    return p;
}
QJsonObject Serialize(const Profile& p) {
    QJsonArray rules,keys,excluded;
    for(const auto& r:p.exclusions) excluded.append(QJsonObject{{"width",int(r.width)},{"height",int(r.height)},
        {"format",int(r.format)},{"sha256",QString::fromStdString(r.hash)},{"context",QString::fromStdString(r.context)}});
    for(const auto& r:p.rules) rules.append(QJsonObject{{"width",int(r.width)},{"height",int(r.height)},
        {"format",int(r.format)},{"sha256",QString::fromStdString(r.hash)},{"context",QString::fromStdString(r.context)}});
    for(auto k:p.keys) keys.append(QString::number(qulonglong(k),16));
    QJsonArray grids;
    for(const auto& g:p.atlas_grids) {
        QJsonArray cells; for(auto cell:g.cells) cells.append(cell);
        QJsonArray rectangles; for(const auto& r:g.rectangles) rectangles.append(QJsonArray{r[0],r[1],r[2],r[3]});
        grids.append(QJsonObject{{"sha256",QString::fromStdString(g.hash)},{"width",int(g.width)},
            {"height",int(g.height)},{"columns",g.columns},{"rows",g.rows},{"cells",cells},{"rectangles",rectangles}});
    }
    QJsonObject result{{"schema",4},{"name",QString::fromStdString(p.name)},{"enabled",p.enabled},
        {"required_context",QString::fromStdString(p.required_context)},
        {"fade_source",QString::fromStdString(p.fade_source)},
        {"fade_channel",p.fade_channel},{"follow_fade",p.follow_fade},
        {"signal_only",p.signal_only},
        {"source_screen",p.source_top ? "top" : "bottom"},
        {"render_mode",p.panel_copy?"panel_copy":p.presentation?"presentation_layer":"guest_draw"},{"scale",p.scale},{"x",p.x},{"y",p.y},
        {"excluded_draws",excluded},
        {"source_crop",QJsonObject{{"u0",p.crop[0]},{"v0",p.crop[1]},{"u1",p.crop[2]},{"v1",p.crop[3]}}},
        {"keys",keys},{"content_rules",rules}};
    result["atlas_selection"]=p.atlas_selection;
    // Null clears old grids when a profile is explicitly converted to whole-texture mode.
    result["atlas_grids"]=p.atlas_selection ? QJsonValue(grids) : QJsonValue();
    return result;
}
bool Write(const QString& path, const QJsonObject& object) {
    QSaveFile file(path); const auto bytes=QJsonDocument(object).toJson();
    return QDir().mkpath(QFileInfo(path).absolutePath()) && file.open(QIODevice::WriteOnly) &&
        file.write(bytes)==bytes.size() && file.commit();
}
}
void Load(const std::string& directory) {
    std::scoped_lock lock(mutex); root=directory; profiles.clear(); captured.clear(); preview_active=false; context_latches.clear();
    notebook_seen = false;
    notebook_detection.store(-1);
    frame_contexts.clear();
    frame_fades.clear();
    last_fades.clear();
    // Import old diagnostic recordings as migration evidence, never as an address fallback.
    std::map<std::uint64_t,std::vector<Rule>> recorded;
    QDir dir(QString::fromStdString(root));
    for(const auto& log:dir.entryList({"*.log"},QDir::Files)) {
        QFile f(dir.filePath(log)); if(!f.open(QIODevice::ReadOnly) || f.size()>8*1024*1024) continue;
        while(!f.atEnd()) {
            const auto line=QString::fromUtf8(f.readLine()); if(!line.startsWith("key=")) continue;
            QMap<QString,QString> fields;
            for(const auto& token:line.trimmed().split(' ',Qt::SkipEmptyParts)) {
                const auto i=token.indexOf('='); if(i>0) fields[token.left(i)]=token.mid(i+1);
            }
            if(fields["feedback"]!="false") continue;
            const auto size=fields["size"].split('x'); if(size.size()!=2) continue;
            const auto context=QString("unit=%1 shader=%2 swizzle=%3 vertices=%4 blend=%5 viewport_raw=%6")
                .arg(fields["unit"],fields["shader"],fields["swizzle"],fields["vertices"],fields["blend"],fields["viewport_raw"]);
            Rule r{size[0].toUInt(),size[1].toUInt(),fields["format"].toUInt(),fields["sha256"].toStdString(),context.toStdString()};
            if(Valid(r)) Add(recorded[fields["key"].mid(2).toULongLong(nullptr,16)],r);
        }
    }
    unsigned unresolved=0;
    for(const auto& folder:dir.entryList(QDir::Dirs|QDir::NoDotAndDotDot)) {
        // Settings snapshots are not renderer profiles.
        if(folder=="settings-backups") continue;
        QDirIterator files(dir.filePath(folder),{"*.json"},QDir::Files,QDirIterator::Subdirectories);
        while(files.hasNext() && profiles.size()<128) {
            const auto path=files.next(); QFile file(path); if(!file.open(QIODevice::ReadOnly) || file.size()>1024*1024) continue;
            auto object=QJsonDocument::fromJson(file.readAll()).object();
            file.close(); // Migration may atomically replace this file on Windows.
            if(object.isEmpty()) continue;
            auto p=Parse(object,dir.relativeFilePath(path).toStdString());
            if(p.rules.empty()) {
                bool complete=!p.keys.empty();
                for(auto k:p.keys) { if(!recorded.contains(k)) complete=false; else for(const auto& r:recorded[k]) Add(p.rules,r); }
                if(!complete) p.rules.clear();
                if(!p.rules.empty()) {
                    const auto backup=path+".before-registry.bak";
                    if(!QFile::exists(backup) && !QFile::copy(path,backup)) { p.rules.clear(); }
                    else {
                        const auto replacement=Serialize(p);
                        for(auto it=replacement.begin();it!=replacement.end();++it) object[it.key()]=it.value();
                        if(!Write(path,object)) p.rules.clear();
                    }
                }
            }
            if(p.rules.empty() && !p.panel_copy) ++unresolved;
            profiles[p.id]=p;
        }
    }
    status=std::to_string(profiles.size())+" profiles loaded; "+std::to_string(unresolved)+" need identity recapture";
}
std::vector<Profile> List() { std::scoped_lock lock(mutex); std::vector<Profile> out; for(const auto& [id,p]:profiles) out.push_back(p); return out; }
std::vector<Profile> Panels() {
    std::scoped_lock lock(mutex); std::vector<Profile> out;
    for (const auto& [id,p]:profiles) if (p.enabled && p.panel_copy) out.push_back(p);
    return out;
}
bool ContextDetectedThisFrame(const std::string& id) {
    std::scoped_lock lock(mutex);
    if (id.empty()) return true;
    const auto it=profiles.find(id);
    return it!=profiles.end() && it->second.enabled && frame_contexts.contains(id);
}
void ObserveFade(const Rule& rule, const std::array<float,7>& alpha) {
    std::scoped_lock lock(mutex);
    for (const auto& [id,p]:profiles) {
        if (!p.enabled || p.panel_copy || std::find(p.rules.begin(),p.rules.end(),rule)==p.rules.end()) continue;
        auto& probe=frame_fades[id];
        if (!probe.draws) probe.minimum=probe.maximum=alpha;
        for (size_t i=0;i<alpha.size();++i) {
            probe.minimum[i]=std::min(probe.minimum[i],alpha[i]);
            probe.maximum[i]=std::max(probe.maximum[i],alpha[i]);
        }
        ++probe.draws;
    }
}
FadeProbe LastFadeProbe(const std::string& id) {
    std::scoped_lock lock(mutex);
    const auto it=last_fades.find(id);
    return it==last_fades.end()?FadeProbe{}:it->second;
}
float FadeOpacity(const Profile& p) {
    if (!p.follow_fade || !p.presentation) return 1.f;
    std::scoped_lock lock(mutex);
    const auto source=profiles.find(p.fade_source);
    if(source==profiles.end() || !source->second.enabled) return 0.f;
    const auto it=frame_fades.find(p.fade_source);
    if(it==frame_fades.end() || !it->second.draws) return 0.f;
    const int channel=std::clamp(p.fade_channel,0,6);
    // Conflicting draws are not a reliable fade signal. Hide rather than choose arbitrarily.
    if(it->second.maximum[channel]-it->second.minimum[channel]>1.f/255.f) return 0.f;
    return std::clamp(it->second.maximum[channel],0.f,1.f);
}
void CompleteDetectionFrame() {
    std::scoped_lock lock(mutex);
    const auto it = profiles.find(notebook_signal);
    const bool configured = it != profiles.end() && it->second.enabled && !it->second.rules.empty();
    notebook_detection.store(configured ? (notebook_seen ? 1 : 0) : -1);
    notebook_seen = false;
    frame_contexts.clear();
    last_fades=std::move(frame_fades);
    frame_fades.clear();
}
bool NotebookDetectedThisFrame() {
    std::scoped_lock lock(mutex);
    const auto it = profiles.find(notebook_signal);
    return notebook_seen && it != profiles.end() && it->second.enabled && !it->second.rules.empty();
}
void Update(const Profile& p) { std::scoped_lock lock(mutex); if(profiles.contains(p.id)) profiles[p.id]=p; }
std::string Save(Profile p) {
    std::scoped_lock lock(mutex);
    if(p.rules.empty() && !p.panel_copy) return "Cannot save without captured hash/signature rules; capture the selected textures again";
    if(p.atlas_selection && std::none_of(p.atlas_grids.begin(),p.atlas_grids.end(),[](const auto& g){return !g.cells.empty() || !g.rectangles.empty();}))
        return "Select at least one grid cell before saving";
    const QString id=QString::fromStdString(p.id);
    if(!QRegularExpression("^[A-Za-z0-9_-]+/[A-Za-z0-9_-]+\\.json$").match(id).hasMatch())
        return "Use letters, numbers, underscore or hyphen for folder/group names";
    const auto path=QDir(QString::fromStdString(root)).filePath(id);
    QJsonObject object; QFile old(path); if(old.open(QIODevice::ReadOnly)) object=QJsonDocument::fromJson(old.readAll()).object();
    old.close(); // Release the Windows read handle before QSaveFile commits.
    const auto fields=Serialize(p); for(auto it=fields.begin();it!=fields.end();++it) object[it.key()]=it.value();
    if(!Write(path,object)) return "Profile save FAILED";
    profiles[p.id]=p; return "Saved "+p.id;
}
bool Candidate(unsigned w,unsigned h,unsigned f) {
    std::scoped_lock lock(mutex);
    if(DrawProbe::Now()<Textures::capture_until.load()) return true;
    auto has=[&](const Profile& p){return p.enabled && std::any_of(p.rules.begin(),p.rules.end(),[&](const Rule& r){return Dimensions(r,w,h,f);});};
    if(InspectionMilliseconds()<Textures::hide_until.load() && has(hidden)) return true;
    if(preview_active && has(preview)) return true;
    for(const auto& [id,p]:profiles) {
        if(has(p)) return true;
        if(p.enabled && p.panel_copy && std::any_of(p.exclusions.begin(),p.exclusions.end(),
            [&](const auto& r){return Dimensions(r,w,h,f);})) return true;
    }
    return false;
}
void Observe(std::uint64_t key,const Rule& r) {
    std::scoped_lock lock(mutex);
    if(!Valid(r) || DrawProbe::Now()>=Textures::capture_until.load()) return;
    if(!captured.contains(key) && captured.size()>=Textures::Limit) return;
    auto& rules=captured[key];
    // Thumbnails are immutable snapshots. Reusing their address must not retarget the selection.
    if(!rules.empty() && rules[0].hash!=r.hash) return;
    Add(rules,r);
}
std::vector<Profile> Match(const Rule& r) {
    std::scoped_lock lock(mutex); std::vector<Profile> out;
    const auto now = DrawProbe::Now();
    auto atlas_overlap=[&](const Profile& p) {
        if (!p.atlas_selection || !draw_uv_valid) return true;
        for (const auto& g:p.atlas_grids) {
            if (g.hash!=r.hash || g.width!=r.width || g.height!=r.height) continue;
            for (const auto& a:g.rectangles)
                if (draw_uv[0]<a[2] && draw_uv[2]>a[0] && draw_uv[1]<a[3] && draw_uv[3]>a[1]) return true;
            for (const int cell:g.cells) {
                const int col=cell%std::max(1,g.columns), row=cell/std::max(1,g.columns);
                const std::array<float,4> a{float(col)/g.columns,float(row)/g.rows,float(col+1)/g.columns,float(row+1)/g.rows};
                if (draw_uv[0]<a[2] && draw_uv[2]>a[0] && draw_uv[1]<a[3] && draw_uv[3]>a[1]) return true;
            }
        }
        return false;
    };
    // Visual atlas transforms are selected per fragment using the shader's actual UVs.
    // CPU bounds used for context observation must not suppress those transforms.
    auto raw_matches=[&](const Profile& p){return p.enabled && std::find(p.rules.begin(),p.rules.end(),r)!=p.rules.end();};
    auto signal_matches=[&](const Profile& p){return raw_matches(p) && atlas_overlap(p);};
    const auto signal = profiles.find(notebook_signal);
    if (signal != profiles.end() && signal_matches(signal->second)) notebook_seen = true;
    for (const auto& [id,p] : profiles)
        if (!p.panel_copy && signal_matches(p)) { context_latches[id] = now; frame_contexts.insert(id); }
    auto matches=[&](const Profile& p) {
        if (!raw_matches(p)) return false;
        if (p.required_context.empty()) return true;
        const auto it = context_latches.find(p.required_context);
        return it != context_latches.end() && now - it->second <= 150;
    };
    auto for_draw=[&](Profile p) {
        if(p.atlas_selection) std::erase_if(p.atlas_grids,[&](const auto& g){return g.hash!=r.hash || g.width!=r.width || g.height!=r.height;});
        return p;
    };
    auto append=[&](const Profile& p) {
        auto filtered=for_draw(p);
        if(filtered.atlas_selection) {
            Atlas::Uniforms mask;
            if(!r.context.starts_with("unit=0 ") || !Atlas::Build(filtered,mask)) return;
        }
        out.push_back(std::move(filtered));
    };
    // Manual picker previews must not inherit a saved profile's visibility gate.
    const bool has_preview=preview_active && raw_matches(preview);
    if(has_preview) append(preview);
    for(const auto& [id,p]:profiles) {
        if (id == notebook_signal || p.panel_copy || p.signal_only) continue;
        bool overridden=has_preview && preview.presentation==p.presentation;
        if(overridden && preview.atlas_selection && p.atlas_selection) {
            Atlas::Uniforms a,b;
            overridden=Atlas::Build(for_draw(preview),a) && Atlas::Build(for_draw(p),b) && Atlas::Overlaps(a,b);
        }
        if(matches(p) && !overridden) append(p);
    }
    if(std::count_if(out.begin(),out.end(),[](const auto& p){return !p.presentation;})>1) ++guest_conflicts;
    return out;
}
void SetDrawTexcoordBounds(float min_x, float min_y, float max_x, float max_y) {
    std::scoped_lock lock(mutex);
    draw_uv={std::clamp(min_x,0.f,1.f),std::clamp(1.f-max_y,0.f,1.f),
             std::clamp(max_x,0.f,1.f),std::clamp(1.f-min_y,0.f,1.f)};
    draw_uv_valid=true;
}
void ClearDrawTexcoordBounds() { std::scoped_lock lock(mutex); draw_uv_valid=false; }
Profile Selection(const std::vector<std::uint64_t>& keys) {
    std::scoped_lock lock(mutex); Profile p; p.id="@preview"; p.keys=keys;
    for(auto key:keys) {
        if(!captured.contains(key) || captured[key].empty()) {p.rules.clear();return p;}
        for(const auto& r:captured[key]) Add(p.rules,r);
    }
    return p;
}
std::vector<std::uint64_t> CapturedKeys(const Profile& p) {
    std::scoped_lock lock(mutex); std::vector<std::uint64_t> keys;
    for(const auto& [key,rules]:captured)
        if(std::any_of(rules.begin(),rules.end(),[&](const Rule& r){return std::find(p.rules.begin(),p.rules.end(),r)!=p.rules.end();})) keys.push_back(key);
    return keys;
}
void Preview(Profile p) { std::scoped_lock lock(mutex); preview=std::move(p); preview.id="@preview"; preview_active=!preview.rules.empty(); }
void StopPreview() { std::scoped_lock lock(mutex); preview_active=false; }
void HideSelection(const std::vector<std::uint64_t>& keys) {
    auto p=Selection(keys); std::scoped_lock lock(mutex); hidden=std::move(p);
}
bool Hidden(const Rule& rule) {
    std::scoped_lock lock(mutex);
    return InspectionMilliseconds()<Textures::hide_until.load() &&
        std::find(hidden.rules.begin(),hidden.rules.end(),rule)!=hidden.rules.end();
}
void ClearCaptured() { std::scoped_lock lock(mutex); captured.clear(); preview_active=false; }
std::string Status() { std::scoped_lock lock(mutex); return status; }
bool Active(const std::string& id) { std::scoped_lock lock(mutex); return id=="@preview" ? preview_active : profiles.contains(id)&&profiles[id].enabled; }
}
