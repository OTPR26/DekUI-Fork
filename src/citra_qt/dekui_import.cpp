// SPDX-License-Identifier: GPL-2.0-or-later
#include <set>
#include <stdexcept>
#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include "citra_qt/configuration/config.h"
#include "citra_qt/dekui_import.h"
#include "common/cityhash.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "common/texture.h"
#include "input_common/main.h"
#include "mm3d/android_menu.h"
#include "mm3d/game_folder.h"
#include "mm3d/player_settings.h"
#include "video_core/rasterizer_cache/texture_codec.h"

namespace MM3D::DesktopImport {
namespace {
void Require(bool good, const QString& message) {
    if (!good)
        throw std::runtime_error(message.toStdString());
}
QByteArray Read(const QString& path, qint64 maximum = 64 * 1024 * 1024) {
    QFile f(path);
    Require(f.open(QIODevice::ReadOnly) && f.size() <= maximum,
            QStringLiteral("Cannot read imported asset"));
    return f.readAll();
}
void Write(const QString& path, const QByteArray& bytes) {
    Require(QDir().mkpath(QFileInfo(path).absolutePath()),
            QStringLiteral("Cannot create output directory"));
    QSaveFile f(path);
    Require(f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit(),
            QStringLiteral("Cannot save imported asset"));
}
QString Hex(u64 hash) {
    return QString::number(hash, 16).rightJustified(16, QLatin1Char('0')).toUpper();
}
void SwordIcon(const QString& stage, const QImage& atlas) {
    Require(atlas.width() == atlas.height() && atlas.width() >= 512 && atlas.width() <= 8192 &&
                atlas.width() % 512 == 0,
            QStringLiteral("Unsupported item icon atlas"));
    const int s = atlas.width() / 512;
    Require(atlas.copy(0, 210 * s, 42 * s, 42 * s)
                .save(stage + QStringLiteral("/master-sword-icon.png")),
            QStringLiteral("Cannot save sword icon"));
}
QString ButtonPath(int index) {
    QSettings ini(NativeSettingsPath(), QSettings::IniFormat);
    const int profile = ini.value(QStringLiteral("Controls/profile"), 0).toInt() + 1;
    const auto prefix = ini.value(QStringLiteral("Controls/profiles/size"), 0).toInt() > 0
                            ? QString(QStringLiteral("Controls/profiles/%1/")).arg(profile)
                            : QString(QStringLiteral("Controls/"));
    return prefix + QString::fromUtf8(Settings::NativeButton::mapping[index]);
}
QString Binding(int index) {
    QSettings ini(NativeSettingsPath(), QSettings::IniFormat);
    return ini
        .value(ButtonPath(index), QString::fromStdString(InputCommon::GenerateKeyboardParam(
                                      QtConfig::default_buttons[index])))
        .toString();
}
void SaveBinding(int index, const QString& binding) {
    QSettings ini(NativeSettingsPath(), QSettings::IniFormat);
    const QString key = ButtonPath(index);
    ini.setValue(key, binding);
    ini.setValue(key + QStringLiteral("/default"), false);
    ini.sync();
}
QString AnalogPath(int index) {
    const auto key = ButtonPath(Settings::NativeButton::A);
    return key.left(key.lastIndexOf(QLatin1Char('/')) + 1) +
           QString::fromStdString(Settings::NativeAnalog::mapping[index]);
}
QString AnalogBinding(int index) {
    QSettings ini(NativeSettingsPath(), QSettings::IniFormat);
    const auto& k = QtConfig::default_analogs[index];
    return ini
        .value(AnalogPath(index), QString::fromStdString(InputCommon::GenerateAnalogParamFromKeys(
                                      k[0], k[1], k[2], k[3], k[4], .5f)))
        .toString();
}
void SaveAnalog(int index, const QString& binding) {
    QSettings ini(NativeSettingsPath(), QSettings::IniFormat);
    ini.setValue(AnalogPath(index), binding);
    ini.setValue(AnalogPath(index) + QStringLiteral("/default"), false);
    ini.sync();
}
QString BindingLabel(const QString& binding) {
    const Common::ParamPackage p(binding.toStdString());
    // SDL's generic face labels are Xbox labels even for a Nintendo controller.
    // Show the physical position so a correctly captured binding is unambiguous.
    if (p.Get("engine", "") == "sdl" && p.Get("api", "") == "controller" && p.Has("button")) {
        const int button = p.Get("button", -1);
        const QStringList face{QStringLiteral("Bottom face button"), QStringLiteral("Right face button"),
                               QStringLiteral("Left face button"), QStringLiteral("Top face button")};
        if (button >= 0 && button < face.size())
            return face[button];
    }
    return p.Get("engine", "") == "keyboard"
               ? QKeySequence(p.Get("code", 0)).toString(QKeySequence::NativeText)
               : QString::fromStdString(InputCommon::ButtonToText(p));
}
class Capture final : public QDialog {
    std::vector<std::unique_ptr<InputCommon::Polling::DevicePoller>> pollers;
    QTimer timer;
    bool analog = false;

public:
    QString binding;
    Capture(QWidget* parent, bool stick = false) : QDialog(parent), analog(stick) {
        setWindowTitle(QStringLiteral("Choose binding"));
        auto* layout = new QVBoxLayout(this);
        layout->addWidget(new QLabel(
            stick ? QStringLiteral("Move the stick horizontally, then vertically. Escape cancels.")
                  : QStringLiteral("Press a controller button or keyboard key. Escape cancels.")));
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        pollers =
            InputCommon::Polling::GetPollers(stick ? InputCommon::Polling::DeviceType::Analog
                                                   : InputCommon::Polling::DeviceType::Button);
        for (auto& poller : pollers)
            poller->Start();
        connect(&timer, &QTimer::timeout, this, [this] {
            for (auto& poller : pollers) {
                auto input = poller->GetNextInput();
                if (input.Has("engine") && !input.Has("down")) {
                    binding = QString::fromStdString(input.Serialize());
                    accept();
                    return;
                }
            }
        });
        timer.start(20);
        resize(460, 140);
    }
    ~Capture() {
        for (auto& poller : pollers)
            poller->Stop();
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape) {
            reject();
            return;
        }
        if (!analog) {
            binding = QString::fromStdString(InputCommon::GenerateKeyboardParam(event->key()));
            accept();
        }
    }
};
} // namespace
QString NativeSettingsPath() {
    const auto root = GameFolder::Root();
    return QFileInfo::exists(root + QStringLiteral("/user/setup/ready.json"))
               ? root + QStringLiteral("/user/config/qt-config.ini")
               : root + QStringLiteral("/desktop-native.ini");
}
void ApplyPendingSettings() {
    const auto root = GameFolder::Root();
    QSettings pending(root + QStringLiteral("/desktop-native.ini"), QSettings::IniFormat),
        actual(root + QStringLiteral("/user/config/qt-config.ini"), QSettings::IniFormat);
    for (const auto& key : pending.allKeys())
        actual.setValue(key, pending.value(key));
    actual.sync();
    Require(actual.status() == QSettings::NoError,
            QStringLiteral("Could not apply startup settings"));
    QFile::remove(root + QStringLiteral("/desktop-native.ini"));
    const QString path = root + QStringLiteral("/desktop-settings.json");
    auto prefs = QFileInfo::exists(path) ? QJsonDocument::fromJson(Read(path, 1024 * 1024)).object()
                                         : QJsonObject{};
    const auto updates = prefs.value(QStringLiteral("pending_settings")).toObject();
    if (!updates.isEmpty()) {
        const QString presentation =
            root + QStringLiteral("/user/config/mm3d-local/presentation.json");
        auto saved = QJsonDocument::fromJson(Read(presentation, 1024 * 1024)).object();
        for (auto it = updates.begin(); it != updates.end(); ++it)
            saved.insert(it.key(), it.value());
        Write(presentation, QJsonDocument(saved).toJson());
        prefs.remove(QStringLiteral("pending_settings"));
        Write(path, QJsonDocument(prefs).toJson());
    }
    SyncTextureSource();
}
void SyncTextureSource() {
    const auto root = GameFolder::Root();
    if (!QFileInfo::exists(root + QStringLiteral("/user/setup/ready.json")))
        return;
    const QString prefs = root + QStringLiteral("/desktop-settings.json");
    QString chosen;
    if (QFileInfo::exists(prefs))
        chosen = QJsonDocument::fromJson(Read(prefs, 1024 * 1024))
                     .object()
                     .value(QStringLiteral("textures"))
                     .toString();
    if (!chosen.isEmpty()) {
        Require(QFileInfo(chosen).isDir(),
                QStringLiteral("Selected texture folder no longer exists"));
        const QStringList candidates{QStringLiteral("/0004000000125500"),
                                     QStringLiteral("/load/textures/0004000000125500"),
                                     QStringLiteral("/textures/0004000000125500")};
        for (const auto& suffix : candidates)
            if (QFileInfo(chosen + suffix).isDir()) {
                chosen += suffix;
                break;
            }
        chosen = QFileInfo(chosen).canonicalFilePath() + QStringLiteral("/");
    }
    Write(root + QStringLiteral("/user/config/mm3d-local/texture-source.json"),
          QJsonDocument(QJsonObject{{QStringLiteral("path"), chosen}}).toJson());
    std::scoped_lock lock{AndroidMenu::mutex};
    AndroidMenu::state.textures = chosen.toStdString();
}
void ChooseTextures(QWidget* parent, bool clear) {
    const auto folder = clear ? QString{}
                              : QFileDialog::getExistingDirectory(
                                    parent, QStringLiteral("Choose your existing MM3D textures"));
    if (!clear && folder.isEmpty())
        return;
    const QString path = GameFolder::Root() + QStringLiteral("/desktop-settings.json");
    auto prefs =
        QFileInfo::exists(path) ? QJsonDocument::fromJson(Read(path)).object() : QJsonObject{};
    prefs.insert(QStringLiteral("textures"), folder);
    Write(path, QJsonDocument(prefs).toJson());
    SyncTextureSource();
}
void RefreshBindings() {
    std::scoped_lock lock{AndroidMenu::mutex};
    AndroidMenu::state.bindings.clear();
    const int buttons[]{Settings::NativeButton::A,      Settings::NativeButton::B,
                        Settings::NativeButton::X,      Settings::NativeButton::Y,
                        Settings::NativeButton::Select, Settings::NativeButton::Start,
                        Settings::NativeButton::Home,   Settings::NativeButton::L,
                        Settings::NativeButton::R,      Settings::NativeButton::ZL,
                        Settings::NativeButton::ZR,     Settings::NativeButton::Up,
                        Settings::NativeButton::Down,   Settings::NativeButton::Left,
                        Settings::NativeButton::Right};
    for (const int i : buttons)
        AndroidMenu::state.bindings.push_back(BindingLabel(Binding(i)).toStdString());
    for (int i = 0; i < 2; ++i)
        AndroidMenu::state.bindings.push_back(
            InputCommon::AnalogToText(Common::ParamPackage(AnalogBinding(i).toStdString()), "up"));
}
bool CaptureBinding(QWidget* parent, unsigned index, bool clear) {
    const int buttons[]{Settings::NativeButton::A,      Settings::NativeButton::B,
                        Settings::NativeButton::X,      Settings::NativeButton::Y,
                        Settings::NativeButton::Select, Settings::NativeButton::Start,
                        Settings::NativeButton::Home,   Settings::NativeButton::L,
                        Settings::NativeButton::R,      Settings::NativeButton::ZL,
                        Settings::NativeButton::ZR,     Settings::NativeButton::Up,
                        Settings::NativeButton::Down,   Settings::NativeButton::Left,
                        Settings::NativeButton::Right};
    if (index >= std::size(buttons)) {
        if (index > 16)
            return false;
        if (clear) {
            SaveAnalog(index - 15, QStringLiteral("engine:null"));
            RefreshBindings();
            return true;
        }
        Capture capture(parent, true);
        if (capture.exec() != QDialog::Accepted)
            return false;
        SaveAnalog(index - 15, capture.binding);
        RefreshBindings();
        return true;
    }
    if (clear) {
        SaveBinding(buttons[index], QStringLiteral("engine:null"));
        RefreshBindings();
        return true;
    }
    Capture capture(parent);
    if (capture.exec() != QDialog::Accepted)
        return false;
    SaveBinding(buttons[index], capture.binding);
    RefreshBindings();
    return true;
}
void MapController(Common::ParamPackage controller) {
    for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
        const int target = i == Settings::NativeButton::L    ? Settings::NativeButton::ZL
                           : i == Settings::NativeButton::R  ? Settings::NativeButton::ZR
                           : i == Settings::NativeButton::ZL ? Settings::NativeButton::L
                           : i == Settings::NativeButton::ZR ? Settings::NativeButton::R
                                                             : i;
        auto mapped = InputCommon::GetControllerButtonBinds(controller, target);
        if (mapped.Has("engine"))
            SaveBinding(i, QString::fromStdString(mapped.Serialize()));
    }
    for (int i = 0; i < 2; ++i) {
        const auto mapped = InputCommon::GetControllerAnalogBinds(controller, i);
        if (mapped.Has("engine"))
            SaveAnalog(i, QString::fromStdString(mapped.Serialize()));
    }
    RefreshBindings();
}
void EnsureController() {
    for (int i = 0; i < Settings::NativeButton::NumButtons; ++i)
        if (Common::ParamPackage(Binding(i).toStdString()).Serialize() !=
            Common::ParamPackage(InputCommon::GenerateKeyboardParam(QtConfig::default_buttons[i]))
                .Serialize())
            return;
    const auto connected = InputCommon::GetConnectedControllers();
    if (!connected.empty())
        MapController(connected.front());
}
void BindingControls(QVBoxLayout* box, QWidget* parent) {
    auto* automatic = new QPushButton(QStringLiteral("Configure connected controller"));
    box->addWidget(automatic);
    QObject::connect(automatic, &QPushButton::clicked, parent, [parent] {
        Capture capture(parent);
        capture.setWindowTitle(QStringLiteral("Press your confirm button (game A)"));
        if (capture.exec() != QDialog::Accepted)
            return;
        const Common::ParamPackage binding(capture.binding.toStdString());
        if (binding.Get("engine", "") != "sdl") {
            QMessageBox::information(parent, QStringLiteral("Controller"),
                                     QStringLiteral("Use a connected controller’s confirm button, "
                                                    "or bind individual controls below."));
            return;
        }
        MapController(binding);
        QMessageBox::information(
            parent, QStringLiteral("Controller configured"),
            QStringLiteral("Controller configured. L1/R1 use items I/II; L2/R2 target and shield. "
                           "Reopen Controls to see the updated bindings."));
    });
    auto* note = new QLabel(QStringLiteral("Bindings use the game’s logical buttons, independently "
                                           "of the controller’s printed labels."));
    note->setWordWrap(true);
    box->addWidget(note);
    const QStringList labels{QStringLiteral("A"),           QStringLiteral("B"),
                             QStringLiteral("X"),           QStringLiteral("Y"),
                             QStringLiteral("D-pad up"),    QStringLiteral("D-pad down"),
                             QStringLiteral("D-pad left"),  QStringLiteral("D-pad right"),
                             QStringLiteral("L — target"),  QStringLiteral("R — shield"),
                             QStringLiteral("Start"),       QStringLiteral("Select"),
                             QStringLiteral("Debug"),       QStringLiteral("GPIO14"),
                             QStringLiteral("ZL — item I"), QStringLiteral("ZR — item II"),
                             QStringLiteral("Home"),        QStringLiteral("Power")};
    for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
        auto* b =
            new QPushButton(labels.value(i, QString::fromUtf8(Settings::NativeButton::mapping[i])) +
                            QStringLiteral(": ") + BindingLabel(Binding(i)));
        box->addWidget(b);
        QObject::connect(b, &QPushButton::clicked, parent, [=] {
            Capture capture(parent);
            if (capture.exec() == QDialog::Accepted) {
                SaveBinding(i, capture.binding);
                b->setText(labels.value(i) + QStringLiteral(": ") + BindingLabel(capture.binding));
            }
        });
    }
    for (int i = 0; i < 2; ++i) {
        auto* b = new QPushButton(i == 0 ? QStringLiteral("Bind left stick")
                                         : QStringLiteral("Bind right stick"));
        box->addWidget(b);
        QObject::connect(b, &QPushButton::clicked, parent, [=] { CaptureBinding(parent, 15 + i); });
    }
}
void VolumeControl(QVBoxLayout* box, bool running) {
    auto* n = new QDoubleSpinBox;
    n->setRange(0, 100);
    n->setDecimals(0);
    QSettings ini(NativeSettingsPath(), QSettings::IniFormat);
    n->setValue(ini.value(QStringLiteral("Audio/volume"), 1.0).toDouble() * 100);
    box->addWidget(new QLabel(QStringLiteral("Volume (%)")));
    box->addWidget(n);
    QObject::connect(n, qOverload<double>(&QDoubleSpinBox::valueChanged), n, [running](double v) {
        if (running)
            PlayerSettings::set_volume.store(int(v));
        else {
            QSettings out(NativeSettingsPath(), QSettings::IniFormat);
            out.setValue(QStringLiteral("Audio/volume"), v / 100.);
            out.setValue(QStringLiteral("Audio/volume/default"), false);
        }
    });
}
void Finalize(const QString& suppliedStage, const QString& root, const QString& pack, bool dark) {
    const QString id = dark ? QStringLiteral("dark-link") : QStringLiteral("adult-link"),
                  parent = root + QStringLiteral("/user/load/mm3d-models");
    const auto stage = QFileInfo(suppliedStage).canonicalFilePath();
    Require(
        !stage.isEmpty() &&
            QFileInfo(stage).absolutePath() == QFileInfo(parent).canonicalFilePath() &&
            QFileInfo(stage).fileName().startsWith(QStringLiteral(".") + id + QStringLiteral("-")),
        QStringLiteral("Invalid generated model directory"));
    QHash<QString, QString> index;
    const QRegularExpression pattern(
        QStringLiteral("^tex1_\\d+x\\d+_([0-9A-Fa-f]{16})_\\d+_mip0\\.(png|dds|ktx)$"),
        QRegularExpression::CaseInsensitiveOption);
    if (!pack.isEmpty()) {
        Require(QFileInfo(pack).isDir(), QStringLiteral("Texture folder no longer exists"));
        QDirIterator files(pack, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                           QDirIterator::Subdirectories);
        int count = 0;
        while (files.hasNext()) {
            const auto path = files.next();
            Require(++count <= 50000 &&
                        QDir(pack).relativeFilePath(path).count(QLatin1Char('/')) < 40,
                    QStringLiteral("Texture folder is too large or too deep"));
            if (!QFileInfo(path).isFile())
                continue;
            const auto match = pattern.match(QFileInfo(path).fileName());
            if (match.hasMatch() && !index.contains(match.captured(1).toUpper()))
                index.insert(match.captured(1).toUpper(), path);
        }
    }
    const auto document = QJsonDocument::fromJson(
        Read(stage + QStringLiteral("/texture-sources/manifest.json"), 1024 * 1024));
    Require(document.isArray(), QStringLiteral("Invalid generated texture manifest"));
    std::set<QString> copied;
    int replacements = 0;
    for (const auto& entry : document.array()) {
        const auto t = entry.toObject();
        const auto relative = t.value(QStringLiteral("file")).toString();
        Require(relative.startsWith(QStringLiteral("texture-sources/")) &&
                    !relative.contains(QStringLiteral("..")) && !QDir::isAbsolutePath(relative),
                QStringLiteral("Invalid texture asset path"));
        const int w = t.value(QStringLiteral("width")).toInt(),
                  h = t.value(QStringLiteral("height")).toInt(),
                  format = t.value(QStringLiteral("format")).toInt();
        Require(w >= 8 && h >= 8 && w <= 4096 && h <= 4096 && w % 8 == 0 && h % 8 == 0 &&
                    (format == 3 || format == 12 || format == 13),
                QStringLiteral("Unsupported generated texture"));
        const auto bytes = Read(stage + QStringLiteral("/") + relative);
        const auto pf = static_cast<VideoCore::PixelFormat>(format);
        const size_t length = size_t(w) * h * VideoCore::GetFormatBpp(pf) / 8;
        Require(size_t(bytes.size()) == length, QStringLiteral("Invalid texture byte count"));
        std::vector<u8> raw(bytes.begin(), bytes.end()),
            linear(size_t(w) * h * VideoCore::GetFormatBytesPerPixel(pf));
        VideoCore::UNSWIZZLE_TABLE[format](w, h, 0, length, linear, raw);
        const auto old = Hex(
                       Common::CityHash64(reinterpret_cast<const char*>(raw.data()), raw.size())),
                   modern = Hex(Common::CityHash64(reinterpret_cast<const char*>(linear.data()),
                                                   linear.size()));
        const auto source = index.value(modern, index.value(old));
        if (t.value(QStringLiteral("icon")).toBool()) {
            Common::FlipRGBA8Texture(linear, w, h);
            SwordIcon(stage, QImage(linear.data(), w, h, QImage::Format_RGBA8888));
            if (source.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
                QImageReader reader(source);
                const auto size = reader.size();
                Require(size.width() == size.height() && size.width() <= 8192 &&
                            size.width() >= 512 && size.width() % 512 == 0,
                        QStringLiteral("Unsupported HD icon atlas"));
                if (size.width() > 4096)
                    reader.setScaledSize(QSize(4096, 4096));
                const auto image = reader.read();
                Require(!image.isNull(), QStringLiteral("Could not decode HD sword icon"));
                SwordIcon(stage, image);
                ++replacements;
            }
        } else if (!source.isEmpty() && copied.insert(old).second) {
            const auto replacement = Read(source);
            for (const auto& hash : std::set<QString>{old, modern})
                Write(stage + QString(QStringLiteral("/texture-overrides/tex1_%1x%2_%3_%4_mip0.%5"))
                                  .arg(w)
                                  .arg(h)
                                  .arg(hash)
                                  .arg(format)
                                  .arg(QFileInfo(source).suffix().toLower()),
                      replacement);
            ++replacements;
        }
    }
    QDir(stage + QStringLiteral("/texture-sources")).removeRecursively();
    auto report = QJsonDocument::fromJson(Read(stage + QStringLiteral("/generated.json"))).object();
    report.insert(QStringLiteral("hd_textures"), replacements);
    Write(stage + QStringLiteral("/generated.json"), QJsonDocument(report).toJson());
    const QString target = parent + QStringLiteral("/") + id,
                  stamp = QString::number(QDateTime::currentMSecsSinceEpoch()),
                  backup = target + QStringLiteral("-backup-") + stamp;
    const QString textures = root + QStringLiteral("/user/load/mm3d-model-textures/0004000000125500"),
                  textureTarget =
                      textures + QStringLiteral("/") + id + QStringLiteral("-generated"),
                  textureBackup =
                      parent + QStringLiteral("/.") + id + QStringLiteral("-textures-backup-") + stamp;
    Require(QDir().mkpath(textures), QStringLiteral("Cannot create texture directory"));
    Require(QDir().mkpath(stage + QStringLiteral("/texture-overrides")),
            QStringLiteral("Cannot stage generated textures"));
    const bool hadModel = QFileInfo::exists(target), hadTextures = QFileInfo::exists(textureTarget);
    bool modelMoved = false, texturesMoved = false, installed = false;
    try {
        if (hadModel) {
            Require(QDir().rename(target, backup), QStringLiteral("Cannot back up current mod"));
            modelMoved = true;
        }
        if (hadTextures) {
            Require(QDir().rename(textureTarget, textureBackup),
                    QStringLiteral("Cannot back up current mod textures"));
            texturesMoved = true;
        }
        Require(QDir().rename(stage, target), QStringLiteral("Cannot install generated mod"));
        installed = true;
        Require(QDir().rename(target + QStringLiteral("/texture-overrides"), textureTarget),
                QStringLiteral("Cannot install generated textures"));
    } catch (...) {
        if (installed)
            QDir().rename(target, stage);
        if (modelMoved)
            QDir().rename(backup, target);
        if (texturesMoved)
            QDir().rename(textureBackup, textureTarget);
        throw;
    }
    const QString config = textures + QStringLiteral("/pack.json");
    if (!QFileInfo::exists(config))
        Write(config,
              R"({"options":{"skip_mipmap":true,"flip_png_files":true,"use_new_hash":true}})");
    QSettings ini(root + QStringLiteral("/user/config/qt-config.ini"), QSettings::IniFormat);
    ini.setValue(QStringLiteral("Renderer/use_disk_shader_cache"), false);
    ini.setValue(QStringLiteral("Renderer/use_disk_shader_cache/default"), false);
    if (replacements) {
        ini.setValue(QStringLiteral("Utility/custom_textures"), true);
        ini.setValue(QStringLiteral("Utility/custom_textures/default"), false);
    }
    ini.sync();
}
} // namespace MM3D::DesktopImport
