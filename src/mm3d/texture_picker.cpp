// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <unordered_set>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <unordered_map>
#include "mm3d/texture_picker.h"
#include "mm3d/draw_probe.h"
#include "mm3d/pause_control.h"
#include "mm3d/profiles.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
namespace MM3D::Textures {
bool TrimVisible(std::span<const std::uint8_t> alpha,unsigned width,unsigned height,
                 std::array<float,4>& r) {
    if(!width || !height || alpha.size()!=size_t(width)*height) return false;
    const auto edge=[](float n,unsigned extent){return std::clamp(int(n*extent+.5f),0,int(extent));};
    const int x0=edge(r[0],width),y0=edge(r[1],height),x1=edge(r[2],width),y1=edge(r[3],height);
    int left=x1,top=y1,right=x0,bottom=y0;
    for(int y=y0;y<y1;++y) for(int x=x0;x<x1;++x) if(alpha[size_t(y)*width+x]!=0) {
        left=std::min(left,x); top=std::min(top,y); right=std::max(right,x+1); bottom=std::max(bottom,y+1);
    }
    if(left>=right || top>=bottom) return false;
    r={float(left)/width,float(top)/height,float(right)/width,float(bottom)/height}; return true;
}
namespace {
std::mutex mutex;
std::unordered_set<std::uint64_t> selected, suppressed, scaled;
std::unordered_set<std::uint64_t> profile_keys, profile_seen;
QJsonArray content_rules;
bool content_enabled = false;
unsigned generation = 0;
std::vector<std::shared_ptr<Thumbnail>> thumbnails;
std::shared_ptr<Thumbnail> detail_image;
std::uint64_t detail_key{};
std::string detail_hash;
std::unordered_set<std::uint64_t> recording_keys;
std::unordered_map<std::uint64_t, std::string> recording_hashes;
std::unordered_set<std::string> recording_rows;
std::string recording_path, recording_status = "No recording yet";
std::int64_t recording_until{};
}
std::string ContentHash(std::span<const std::uint8_t> bytes) {
    return bytes.empty() ? std::string{} : QCryptographicHash::hash(
        QByteArrayView(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
        QCryptographicHash::Sha256).toHex().toStdString();
}
bool ContentCandidate(unsigned width, unsigned height, unsigned format) {
    std::scoped_lock lock{mutex};
    if (!content_enabled || !profile_enabled.load()) return false;
    for (const auto& value : content_rules) {
        const auto r = value.toObject();
        if (r["width"].toInt() == int(width) && r["height"].toInt() == int(height) &&
            r["format"].toInt() == int(format)) return true;
    }
    return false;
}
bool ContentMatches(unsigned width, unsigned height, unsigned format,
                    const std::string& hash, const std::string& context) {
    std::scoped_lock lock{mutex};
    if (!content_enabled || !profile_enabled.load() || hash.empty()) return false;
    for (const auto& value : content_rules) {
        const auto r = value.toObject();
        if (r["width"].toInt() == int(width) && r["height"].toInt() == int(height) &&
            r["format"].toInt() == int(format) && r["sha256"].toString().toStdString() == hash &&
            r["context"].toString().toStdString() == context) return true;
    }
    return false;
}

// Addresses in these recordings select migration inputs, never runtime identity.
static QJsonArray RecordedRules(const QString& path, const QJsonArray& keys) {
    QJsonArray result;
    const QDir root(QFileInfo(path).dir().absoluteFilePath(".."));
    for (const auto* name : {"logo-intro.log", "logo-return.log"}) {
        QFile log(root.filePath(name));
        if (!log.open(QIODevice::ReadOnly)) continue;
        for (const auto& line : QString::fromUtf8(log.readAll()).split('\n')) {
            if (!line.startsWith("key=")) continue;
            QMap<QString, QString> fields;
            for (const auto& token : line.split(' ', Qt::SkipEmptyParts)) {
                const auto i = token.indexOf('=');
                if (i > 0) fields[token.left(i)] = token.mid(i + 1);
            }
            if (!keys.contains(fields["key"].mid(2)) || fields["sha256"].size() != 64 ||
                fields["feedback"] != "false") continue;
            const auto size = fields["size"].split('x');
            if (size.size() != 2) continue;
            const QString context = QString("unit=%1 shader=%2 swizzle=%3 vertices=%4 blend=%5 viewport_raw=%6")
                .arg(fields["unit"], fields["shader"], fields["swizzle"], fields["vertices"],
                     fields["blend"], fields["viewport_raw"]);
            const QJsonObject rule{{"width", size[0].toInt()}, {"height", size[1].toInt()},
                {"format", fields["format"].toInt()}, {"sha256", fields["sha256"]}, {"context", context}};
            if (!result.contains(rule)) result.append(rule);
        }
    }
    return result;
}
bool StartRecording(const std::string& path, const std::string& phase) {
    std::scoped_lock lock{mutex};
    if (selected.empty()) { recording_status = "Select textures first"; return false; }
    QFile file(QString::fromStdString(path));
    const QByteArray header = (QDateTime::currentDateTimeUtc().toString(Qt::ISODate) +
        " Phase: " + QString::fromStdString(phase) +
        "\nHash: SHA256 of guest encoded base-level bytes at first observed binding.\n"
        "No texture pixels exported. Same artwork can have different encodings.\n").toUtf8();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append) || file.write(header) != header.size()) {
        recording_status = "Cannot write recording log"; return false;
    }
    recording_keys = selected; recording_hashes.clear(); recording_rows.clear();
    recording_path = path; recording_until = InspectionMilliseconds() + 5000;
    recording_status = "Recording selected draws; resume if paused";
    return true;
}
bool RecordingKey(std::uint64_t key) {
    std::scoped_lock lock{mutex};
    return InspectionMilliseconds() < recording_until && recording_rows.size() < 128 && recording_keys.contains(key);
}
bool NeedsRecordingHash(std::uint64_t key) {
    std::scoped_lock lock{mutex}; return !recording_hashes.contains(key);
}
void RecordBinding(std::uint64_t key, std::span<const std::uint8_t> bytes, const std::string& context) {
    std::scoped_lock lock{mutex};
    if (InspectionMilliseconds() >= recording_until || recording_rows.size() >= 128 || !recording_keys.contains(key)) return;
    auto [it, inserted] = recording_hashes.try_emplace(key);
    if (inserted) it->second = bytes.empty() ? "unavailable" : QCryptographicHash::hash(
        QByteArrayView(reinterpret_cast<const char*>(bytes.data()), bytes.size()), QCryptographicHash::Sha256).toHex().toStdString();
    const auto row = "key=0x" + QString::number(qulonglong(key), 16).toStdString() +
        " sha256=" + it->second + " " + context;
    if (!recording_rows.insert(row).second) return;
    QFile file(QString::fromStdString(recording_path));
    const auto output = QByteArray::fromStdString(row + "\n");
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append) || file.write(output) != output.size()) {
        recording_status = "Recording write failed"; recording_until = 0; return;
    }
    recording_status = "Saved " + std::to_string(recording_rows.size()) + " unique bindings";
}
std::string RecordingStatus() {
    std::scoped_lock lock{mutex}; return recording_status;
}
Thumbnail::~Thumbnail() {
    device.destroyImageView(view); device.destroyImage(image); device.freeMemory(memory);
}
std::uint64_t Key(unsigned address, unsigned width, unsigned height, unsigned format) {
    return std::uint64_t(address) | (std::uint64_t(width & 2047) << 32) |
        (std::uint64_t(height & 2047) << 43) | (std::uint64_t(format & 31) << 54);
}
void Start() { capture_until.store(DrawProbe::Now() + 5000); }
void RequestDetail(std::uint64_t key,const std::string& hash) {
    std::scoped_lock lock{mutex};
    if(detail_key==key && detail_hash==hash) return;
    detail_key=key; detail_hash=hash; detail_image.reset();
}
bool WantsDetail(std::uint64_t key,const std::string& hash) {
    std::scoped_lock lock{mutex};
    return key && key==detail_key && !detail_image && (hash.empty() || hash==detail_hash);
}
std::shared_ptr<Thumbnail> Detail() { std::scoped_lock lock{mutex}; return detail_image; }
bool Wants(std::uint64_t key) {
    if (!key || DrawProbe::Now() >= capture_until.load()) return false;
    std::scoped_lock lock{mutex};
    return thumbnails.size() < Limit && std::none_of(thumbnails.begin(), thumbnails.end(),
        [key](const auto& item) { return item->key == key; });
}
std::vector<std::shared_ptr<Thumbnail>> Snapshot() {
    std::scoped_lock lock{mutex}; return thumbnails;
}
void Clear() {
    Profiles::ClearCaptured();
    capture_until.store(0); hide_until.store(0); hidden.store(0);
    hide_by_address.store(false);
    scale_until.store(0); scaled_draws.store(0);
    profile_active.store(false);
    profile_enabled.store(true);
    std::scoped_lock lock{mutex}; ++generation; thumbnails.clear(); detail_image.reset(); detail_key=0; detail_hash.clear();
    selected.clear(); suppressed.clear(); scaled.clear();
    recording_until = 0; recording_keys.clear(); recording_hashes.clear(); recording_rows.clear();
}
void ToggleSelection(std::uint64_t key) {
    if (!key) return;
    std::scoped_lock lock{mutex};
    if (!selected.erase(key)) selected.insert(key);
}
void SetSelection(const std::vector<std::uint64_t>& keys) {
    std::scoped_lock lock{mutex}; selected = {keys.begin(),keys.end()};
}
bool IsSelected(std::uint64_t key) {
    std::scoped_lock lock{mutex}; return selected.contains(key);
}
unsigned SelectionCount() {
    std::scoped_lock lock{mutex}; return unsigned(selected.size());
}
std::vector<std::uint64_t> SelectedKeys() {
    std::scoped_lock lock{mutex}; return {selected.begin(), selected.end()};
}
bool LoadProfile(const std::string& path) {
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    const auto values = object.value("keys").toArray();
    if (values.isEmpty()) return false;
    std::unordered_set<std::uint64_t> wanted;
    for (const auto& value : values) wanted.insert(value.toString().toULongLong(nullptr, 16));
    std::scoped_lock lock{mutex};
    for (const auto key : wanted)
        if (std::none_of(thumbnails.begin(), thumbnails.end(), [key](const auto& item) {
                return item->key == key;
            })) return false;
    selected = std::move(wanted);
    group_scale.store(float(object.value("scale").toDouble(.5)));
    group_x.store(float(object.value("x").toDouble()));
    group_y.store(float(object.value("y").toDouble()));
    return true;
}
void DisableProfile() {
    std::scoped_lock lock{mutex};
    profile_active.store(false); content_enabled = false; profile_keys.clear();
    profile_enabled.store(false);
}
bool LoadProfileDefinition(const std::string& path) {
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) return false;
    auto object = QJsonDocument::fromJson(file.readAll()).object();
    file.close(); // Windows requires readers closed before atomic replacement during migration.
    const auto values = object.value("keys").toArray();
    if (values.isEmpty()) return false;
    auto rules = object.value("content_rules").toArray();
    if (rules.isEmpty() && QFileInfo(file).fileName() == "title_logo.json") {
        rules = RecordedRules(QString::fromStdString(path), values);
        if (!rules.isEmpty()) {
            object["schema"] = 2; object["content_rules"] = rules;
            const auto backup = QString::fromStdString(path) + ".schema1.bak";
            if (!QFile::exists(backup) && !QFile::copy(QString::fromStdString(path), backup)) return false;
            QSaveFile save(QString::fromStdString(path));
            const auto bytes = QJsonDocument(object).toJson();
            if (!save.open(QIODevice::WriteOnly) || save.write(bytes) != bytes.size() || !save.commit())
                return false;
        }
    }
    std::scoped_lock lock{mutex};
    profile_enabled.store(object.value("enabled").toBool(true));
    content_rules = rules; content_enabled = !rules.isEmpty();
    profile_keys.clear(); profile_seen.clear(); profile_active.store(false);
    for (const auto& value : values) profile_keys.insert(value.toString().toULongLong(nullptr, 16));
    saved_scale.store(float(object.value("scale").toDouble(.5)));
    saved_x.store(float(object.value("x").toDouble())); saved_y.store(float(object.value("y").toDouble()));
    return true;
}
void ObserveProfileKey(std::uint64_t key) {
    std::scoped_lock lock{mutex};
    if (!content_rules.isEmpty()) return; // No address fallback for content profiles.
    if (!profile_enabled.load() || profile_keys.empty() || !profile_keys.contains(key)) return;
    profile_seen.insert(key);
}
bool SavedScaleMatches(std::uint64_t key) {
    std::scoped_lock lock{mutex};
    return profile_enabled.load() && content_rules.isEmpty() && !profile_keys.empty() &&
        profile_seen.size() == profile_keys.size() && profile_keys.contains(key);
}
std::string ProfileReport(const std::string& path) {
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) return "profile file not found";
    const auto values = QJsonDocument::fromJson(file.readAll()).object().value("keys").toArray();
    unsigned matched = 0; std::string missing;
    for (const auto& value : values) {
        const auto key = value.toString().toULongLong(nullptr, 16);
        bool found = false;
        { std::scoped_lock lock{mutex};
          found = std::any_of(thumbnails.begin(), thumbnails.end(), [key](const auto& t) { return t->key == key; }); }
        if (found) ++matched;
        else missing += " 0x" + value.toString().toStdString();
    }
    return "profile matches " + std::to_string(matched) + "/" + std::to_string(values.size()) +
           (missing.empty() ? "" : "; missing:" + missing);
}
bool Matches(std::uint64_t key) {
    if (!key || InspectionMilliseconds() >= hide_until.load()) return false;
    std::scoped_lock lock{mutex}; return suppressed.contains(key);
}
void HideSelected(bool by_address) {
    {
        std::scoped_lock lock{mutex};
        if (selected.empty()) return;
        suppressed = selected;
    }
    hidden.store(0);
    hide_by_address.store(by_address);
    scale_until.store(0);
    Profiles::HideSelection(by_address ? std::vector<std::uint64_t>{} : SelectedKeys());
    hide_until.store(InspectionMilliseconds() + 5000);
    if (inspection_paused.load()) step_requested.store(true);
}
void ScaleSelected() {
    {
        std::scoped_lock lock{mutex};
        if (selected.empty()) return;
        scaled = selected;
    }
    hide_until.store(0);
    scaled_draws.store(0);
    scale_until.store(InspectionMilliseconds() + 5000);
    profile_active.store(false);
    if (inspection_paused.load()) step_requested.store(true);
}
void ApplyProfile() {
    {
        std::scoped_lock lock{mutex};
        if (selected.empty()) return;
        scaled = selected;
    }
    scaled_draws.store(0);
    profile_active.store(true);
}
bool ScaleMatches(std::uint64_t key) {
    if (!key || (!profile_active.load() && InspectionMilliseconds() >= scale_until.load())) return false;
    std::scoped_lock lock{mutex}; return scaled.contains(key);
}
void Restore() {
    Profiles::StopPreview();
    hide_until.store(0);
    hide_by_address.store(false);
    scale_until.store(0);
    profile_active.store(false);
    if (inspection_paused.load()) step_requested.store(true);
}
void Capture(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler, vk::Image source,
             vk::Format format, unsigned width, unsigned height, std::uint64_t key,
             vk::ImageLayout source_layout, bool detail) {
    if (!width || !height || (detail ? !WantsDetail(key) : !Wants(key))) return;
    if(detail && (width>4096 || height>4096)) return;
    unsigned capture_generation;
    { std::scoped_lock lock{mutex}; capture_generation = generation; }
    const auto physical = instance.GetPhysicalDevice();
    if (!(physical.getFormatProperties(format).optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitSrc) ||
        !(physical.getFormatProperties(vk::Format::eR8G8B8A8Unorm).optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst)) return;
    auto item = std::make_shared<Thumbnail>();
    item->device = instance.GetDevice(); item->key = key; item->width = width; item->height = height;
    const float scale = detail ? 1.f : std::min(1.f, 192.f / std::max(width, height));
    const unsigned w = std::max(1u, unsigned(width * scale)), h = std::max(1u, unsigned(height * scale));
    item->image = item->device.createImage({.imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc});
    const auto requirements = item->device.getImageMemoryRequirements(item->image);
    const auto properties = physical.getMemoryProperties();
    unsigned type = properties.memoryTypeCount;
    for (unsigned i = 0; i < properties.memoryTypeCount; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) { type = i; break; }
    if (type == properties.memoryTypeCount) return;
    item->memory = item->device.allocateMemory({.allocationSize = requirements.size, .memoryTypeIndex = type});
    item->device.bindImageMemory(item->image, item->memory, 0);
    item->view = item->device.createImageView({.image = item->image, .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
    scheduler.Record([source, source_layout, item, width, height, w, h](vk::CommandBuffer command) {
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        const std::array before{
            vk::ImageMemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead, .oldLayout = source_layout,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source, .subresourceRange = range},
            vk::ImageMemoryBarrier{.dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = item->image, .subresourceRange = range}};
        command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, before);
        const vk::ImageBlit blit{.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .srcOffsets = std::array{vk::Offset3D{0,0,0}, vk::Offset3D{int(width),int(height),1}},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstOffsets = std::array{vk::Offset3D{0,0,0}, vk::Offset3D{int(w),int(h),1}}};
        command.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, item->image,
            vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eNearest);
        const std::array after{
            vk::ImageMemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal, .newLayout = source_layout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source, .subresourceRange = range},
            vk::ImageMemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = vk::AccessFlagBits::eShaderRead,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal, .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = item->image, .subresourceRange = range}};
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, after);
    });
    // Debug capture only: publish an immutable thumbnail after its copy completes.
    // Do not expose renderer-cache image views to the presentation thread.
    scheduler.Finish();
    if(detail) {
        // One CPU readback per full-resolution capture. GUI trimming then uses only RAM.
        vk::UniqueDeviceMemory read_memory;
        auto buffer=item->device.createBufferUnique({.size=vk::DeviceSize(w)*h*4,
            .usage=vk::BufferUsageFlagBits::eTransferDst,.sharingMode=vk::SharingMode::eExclusive});
        const auto req=item->device.getBufferMemoryRequirements(*buffer);
        unsigned host_type=properties.memoryTypeCount;
        const auto flags=vk::MemoryPropertyFlagBits::eHostVisible|vk::MemoryPropertyFlagBits::eHostCoherent;
        for(unsigned i=0;i<properties.memoryTypeCount;++i)
            if((req.memoryTypeBits&(1u<<i)) && (properties.memoryTypes[i].propertyFlags&flags)==flags) {host_type=i;break;}
        if(host_type<properties.memoryTypeCount) {
            read_memory=item->device.allocateMemoryUnique({.allocationSize=req.size,.memoryTypeIndex=host_type});
            item->device.bindBufferMemory(*buffer,*read_memory,0);
            scheduler.Record([item,target=*buffer,w,h](vk::CommandBuffer command) {
                vk::ImageMemoryBarrier barrier{.srcAccessMask=vk::AccessFlagBits::eShaderRead,
                    .dstAccessMask=vk::AccessFlagBits::eTransferRead,
                    .oldLayout=vk::ImageLayout::eShaderReadOnlyOptimal,.newLayout=vk::ImageLayout::eTransferSrcOptimal,
                    .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                    .image=item->image,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
                command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,{}, {},{},barrier);
                command.copyImageToBuffer(item->image,vk::ImageLayout::eTransferSrcOptimal,target,
                    vk::BufferImageCopy{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={w,h,1}});
                barrier.srcAccessMask=vk::AccessFlagBits::eTransferRead; barrier.dstAccessMask=vk::AccessFlagBits::eShaderRead;
                barrier.oldLayout=vk::ImageLayout::eTransferSrcOptimal; barrier.newLayout=vk::ImageLayout::eShaderReadOnlyOptimal;
                const vk::MemoryBarrier host{.srcAccessMask=vk::AccessFlagBits::eTransferWrite,.dstAccessMask=vk::AccessFlagBits::eHostRead};
                command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eAllCommands|vk::PipelineStageFlagBits::eHost,{},host,{},barrier);
            });
            scheduler.Finish();
            const auto* rgba=static_cast<const std::uint8_t*>(item->device.mapMemory(*read_memory,0,req.size));
            item->alpha.resize(size_t(w)*h);
            for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x)
                item->alpha[size_t(y)*w+x]=rgba[(size_t(h-1-y)*w+x)*4+3];
            item->device.unmapMemory(*read_memory);
        }
    }
    std::scoped_lock lock{mutex};
    if(detail) {
        if(capture_generation==generation && detail_key==key) detail_image=std::move(item);
    } else if (capture_generation == generation && capture_until.load() != 0)
        thumbnails.push_back(std::move(item));
}
}
