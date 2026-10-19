// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include "common/scm_rev.h"
#include <functional>
#include <stdexcept>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include "citra_qt/dekui_frontend.h"
#include "citra_qt/dekui_import.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "core/frontend/input.h"
#include "input_common/main.h"
#include "mm3d/camera_distance.h"
#include "mm3d/diagnostics.h"
#include "mm3d/game_folder.h"
#include "mm3d/game_setup.h"
#include "mm3d/player_settings.h"

namespace MM3D::DesktopFrontend {
namespace {
const char* Theme = R"(
QDialog,QWidget#Startup{background:#08070d;color:#f2f2f2;font-family:'Liberation Sans';font-size:18px;}
QLabel{color:#eee;background:transparent;} QLabel#Heading{color:#b97df2;font-size:24px;}
QPushButton,QComboBox,QDoubleSpinBox{color:#eee;background:#291c38;border:0;border-radius:5px;padding:10px;min-height:24px;}
QPushButton:hover,QPushButton:focus,QComboBox:focus{background:#3d1468;color:#d9b5ff;}
QComboBox QAbstractItemView{background:#17111f;color:#f2f2f2;alternate-background-color:#17111f;selection-background-color:#3d1468;selection-color:#ffffff;border:1px solid #76548d;outline:0;}
QComboBox QAbstractItemView::item{background:#17111f;color:#f2f2f2;padding:8px;min-height:24px;}
QComboBox QAbstractItemView::item:selected{background:#3d1468;color:#ffffff;}
QComboBox QAbstractItemView::item:disabled{color:#9b90a6;}
QPushButton:disabled{color:#777;background:#1b1721;}
QListWidget{background:#08070d;color:#eee;border:0;outline:0;min-width:210px;}
QListWidget::item{padding:12px;border-radius:4px;margin-bottom:8px;background:#291c38;}
QListWidget::item:selected{background:#3d1468;}
QScrollArea,QWidget#Panel{border:0;background:#100d17;}
QGroupBox{color:#b97df2;background:#17111f;border:0;border-radius:5px;margin-top:18px;padding:12px;} QGroupBox::title{subcontrol-origin:margin;left:12px;}
QCheckBox{color:#eee;padding:10px;} QCheckBox::indicator{width:24px;height:24px;}
QScrollBar:vertical{background:#15101d;width:18px;margin:0;} QScrollBar::handle:vertical{background:#76548d;min-height:50px;border-radius:7px;}
QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}
)";
QString Root() {
    return GameFolder::Root();
}
QJsonObject Read(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object()
                                       : QJsonObject{};
}
void Write(const QString& path, const QJsonObject& obj) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(obj).toJson()) < 0 || !f.commit())
        throw std::runtime_error("Could not save settings");
}
QString PrefsPath() {
    return Root() + QStringLiteral("/desktop-settings.json");
}
QString PresentationPath() {
    return Root() + QStringLiteral("/user/config/mm3d-local/presentation.json");
}
QJsonObject Presentation() {
    auto defaults = Read(GameFolder::PackageRoot() +
                         QStringLiteral("/resources/mm3d/defaults/presentation.json"));
    const auto saved = Read(PresentationPath());
    for (auto i = saved.begin(); i != saved.end(); ++i)
        defaults.insert(i.key(), i.value());
    const auto pending = Read(PrefsPath()).value(QStringLiteral("pending_settings")).toObject();
    for (auto i = pending.begin(); i != pending.end(); ++i)
        defaults.insert(i.key(), i.value());
    return defaults;
}
void Set(const QString& key, const QJsonValue& value) {
    if (QFileInfo::exists(Root() + QStringLiteral("/user/setup/ready.json"))) {
        auto obj = Read(PresentationPath());
        obj.insert(key, value);
        Write(PresentationPath(), obj);
    } else {
        auto prefs = Read(PrefsPath());
        auto pending = prefs.value(QStringLiteral("pending_settings")).toObject();
        pending.insert(key, value);
        prefs.insert(QStringLiteral("pending_settings"), pending);
        Write(PrefsPath(), prefs);
    }
}
void SetPreference(const QString& key, const QString& value) {
    auto obj = Read(PrefsPath());
    obj.insert(key, value);
    Write(PrefsPath(), obj);
    if (key == QStringLiteral("textures"))
        DesktopImport::SyncTextureSource();
}
QPushButton* Button(QVBoxLayout* box, const QString& text, std::function<void()> callback) {
    auto* b = new QPushButton(text);
    b->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    box->addWidget(b, 0, Qt::AlignLeft);
    QObject::connect(b, &QPushButton::clicked, b, std::move(callback));
    return b;
}
void Note(QVBoxLayout* box, const QString& text) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color:#b5a9c2;padding:8px;"));
    box->addWidget(label);
}
void Heading(QVBoxLayout* box, const QString& text) {
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("Heading"));
    box->addWidget(label);
}
// Resolve logical buttons through the saved profile, rather than guessing Xbox/Nintendo.
class MenuInput final : public QObject {
    QTimer timer;
    QElapsedTimer clock;
    std::array<std::unique_ptr<Input::ButtonDevice>, 8> devices;
    std::array<bool, 8> previous{};
    std::array<qint64, 8> repeat{};
    QString signature;

public:
    MenuInput(QObject* parent) : QObject(parent) {
        clock.start();
        connect(&timer, &QTimer::timeout, this, [this] {
            auto* dialog = QApplication::activeModalWidget();
            if (!dialog || dialog->property("dekuiMenu").toBool() == false)
                return;
            QSettings ini(DesktopImport::NativeSettingsPath(), QSettings::IniFormat);
            const int profile = ini.value(QStringLiteral("Controls/profile"), 0).toInt() + 1;
            const QString prefix =
                ini.value(QStringLiteral("Controls/profiles/size"), 0).toInt() > 0
                    ? QStringLiteral("Controls/profiles/%1/").arg(profile)
                    : QStringLiteral("Controls/");
            constexpr int logical[]{Settings::NativeButton::A,     Settings::NativeButton::B,
                                    Settings::NativeButton::Start, Settings::NativeButton::Up,
                                    Settings::NativeButton::Down,  Settings::NativeButton::Left,
                                    Settings::NativeButton::Right, Settings::NativeButton::Select};
            QStringList saved;
            for (int i : logical)
                saved << ini.value(prefix + QString::fromUtf8(Settings::NativeButton::mapping[i]),
                                   QStringLiteral("engine:null"))
                             .toString();
            const auto current = saved.join(QLatin1Char('|'));
            if (current != signature) {
                signature = current;
                for (int i = 0; i < 8; ++i)
                    devices[i] = Input::CreateDevice<Input::ButtonDevice>(saved[i].toStdString());
                previous.fill(true);
            }
            for (int i = 0; i < 8; ++i) {
                const bool down = devices[i] && devices[i]->GetStatus();
                const bool trigger =
                    down && (!previous[i] || (i >= 3 && i <= 6 && clock.elapsed() >= repeat[i]));
                if (trigger) {
                    repeat[i] = clock.elapsed() + (previous[i] ? 110 : 400);
                    const int key = i == 0 || i == 2   ? Qt::Key_Return
                                    : i == 1 || i == 7 ? Qt::Key_Escape
                                    : i == 3           ? Qt::Key_Up
                                    : i == 4           ? Qt::Key_Down
                                    : i == 5           ? Qt::Key_Left
                                                       : Qt::Key_Right;
                    auto* target = QApplication::focusWidget();
                    if (!target)
                        target = dialog;
                    if ((key == Qt::Key_Up || key == Qt::Key_Down) &&
                        qobject_cast<QPushButton*>(target)) {
                        QKeyEvent press(QEvent::KeyPress, Qt::Key_Tab,
                                        key == Qt::Key_Up ? Qt::ShiftModifier : Qt::NoModifier);
                        QApplication::sendEvent(target, &press);
                    } else {
                        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier),
                            release(QEvent::KeyRelease, key, Qt::NoModifier);
                        QApplication::sendEvent(target, &press);
                        QApplication::sendEvent(target, &release);
                    }
                }
                previous[i] = down;
            }
        });
        timer.start(30);
    }
};
class SettingsDialog final : public QDialog {
    QListWidget* rail{};
    QScrollArea* scroll{};
    QVBoxLayout* box{};
    bool running;
    QString page = QStringLiteral("General");
    void Bool(const QString& label, const QString& key, bool fallback = false) {
        auto* c = new QCheckBox(label);
        c->setChecked(Presentation().value(key).toBool(fallback));
        box->addWidget(c);
        connect(c, &QCheckBox::toggled, this, [key](bool value) { Set(key, value); });
    }
    void Number(const QString& label, const QString& key, double fallback, double min, double max,
                double step = 1) {
        auto* row = new QHBoxLayout;
        auto* caption = new QLabel(label);
        caption->setWordWrap(true);
        row->addWidget(caption, 1);
        auto* n = new QDoubleSpinBox;
        n->setRange(min, max);
        n->setSingleStep(step);
        n->setDecimals(step < 1 ? 2 : 0);
        n->setValue(Presentation().value(key).toDouble(fallback));
        row->addWidget(n);
        box->addLayout(row);
        connect(n, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [key](double v) { Set(key, v); });
    }
    void Choice(const QString& label, const QStringList& choices, int current,
                std::function<void(int)> callback) {
        box->addWidget(new QLabel(label));
        auto* combo = new QComboBox;
        combo->addItems(choices);
        combo->setCurrentIndex(current);
        box->addWidget(combo);
        connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, std::move(callback));
    }
    void NativeBool(const QString& label, const QString& section, const QString& key, bool fallback,
                    std::atomic<int>* command = nullptr) {
        QSettings ini(DesktopImport::NativeSettingsPath(), QSettings::IniFormat);
        auto* c = new QCheckBox(label);
        c->setChecked(ini.value(section + QStringLiteral("/") + key, fallback).toBool());
        box->addWidget(c);
        connect(c, &QCheckBox::toggled, this, [=, this](bool value) {
            if (running && command) {
                command->store(value);
                return;
            }
            QSettings out(DesktopImport::NativeSettingsPath(), QSettings::IniFormat);
            out.setValue(section + QStringLiteral("/") + key, value);
            out.setValue(section + QStringLiteral("/") + key + QStringLiteral("/default"), false);
            out.sync();
        });
    }
    void Generate(bool dark, bool twilight = false) {
        if (running) {
            QMessageBox::information(
                this, QStringLiteral("Mod setup"),
                QStringLiteral("Return to the startup screen before generating a mod."));
            return;
        }
        const auto prefs = Read(PrefsPath());
        const auto mm = GameFolder::Rom();
        const auto oot =
            prefs.value(twilight ? QStringLiteral("tp_rom") : QStringLiteral("oot_rom")).toString();
        if (mm.isEmpty() || oot.isEmpty()) {
            QMessageBox::information(
                this, QStringLiteral("Mod setup"),
                QStringLiteral("Select your MM3D ROM and the source ROM for this mod first."));
            return;
        }
        if (!QFileInfo::exists(Root() + QStringLiteral("/user/setup/ready.json"))) {
            const auto error = GameFolder::Prepare();
            if (!error.isEmpty()) {
                QMessageBox::warning(this, QStringLiteral("Game setup"), error);
                return;
            }
            DesktopImport::ApplyPendingSettings();
        }
        QProgressDialog progress(QStringLiteral("Preparing…"), QString{}, 0, 0, this);
        progress.setWindowTitle(twilight ? QStringLiteral("Creating Twilight Princess Link")
                                : dark   ? QStringLiteral("Creating Dark Link")
                                         : QStringLiteral("Creating Adult Link"));
        progress.setWindowModality(Qt::WindowModal);
        progress.setCancelButton(nullptr);
        progress.show();
#ifdef Q_OS_MACOS
        const QString python =
            QDir(QCoreApplication::applicationDirPath())
                .absoluteFilePath(QStringLiteral("../Resources/runtime/python/bin/python3"));
#elif defined(Q_OS_WIN)
        const QString python = Root() + QStringLiteral("/runtime/python/python.exe");
#else
        const QString python =
            GameFolder::PackageRoot() + QStringLiteral("/runtime/tp-python/python/bin/python3");
#endif
        QProcess process;
        QEventLoop loop;
        QByteArray pending;
        QJsonObject result;
        connect(&process, &QProcess::readyReadStandardOutput, this, [&] {
            pending += process.readAllStandardOutput();
            while (pending.contains('\n')) {
                const auto at = pending.indexOf('\n');
                const auto line = QJsonDocument::fromJson(pending.left(at)).object();
                pending.remove(0, at + 1);
                if (line.contains(QStringLiteral("progress")))
                    progress.setLabelText(line.value(QStringLiteral("progress")).toString());
                if (line.contains(QStringLiteral("stage")))
                    result = line;
            }
        });
        connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &loop,
                &QEventLoop::quit);
        connect(&process, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
        QStringList args{QStringLiteral("-E"),
                         QStringLiteral("-s"),
                         QStringLiteral("-B"),
                         GameFolder::PackageRoot() +
                             (twilight ? QStringLiteral("/tools/mm3d/generate_tp_mod.py")
                                       : QStringLiteral("/tools/mm3d/mod_import/generate.py")),
                         QStringLiteral("--root"),
                         Root(),
                         QStringLiteral("--mm-rom"),
                         mm,
                         twilight ? QStringLiteral("--tp-rom") : QStringLiteral("--oot-rom"),
                         oot};
        if (twilight)
            args << QStringLiteral("--package") << GameFolder::PackageRoot();
        if (dark)
            args << QStringLiteral("--dark");
        process.start(python, args);
        loop.exec();
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 ||
            !result.contains(QStringLiteral("stage"))) {
            QMessageBox::warning(this, QStringLiteral("Mod setup"),
                                 QString::fromUtf8(process.readAllStandardError()).right(2000));
            return;
        }
        try {
            progress.setLabelText(QStringLiteral("Importing matching textures…"));
            QApplication::processEvents();
            const auto stage = result.value(QStringLiteral("stage")).toString();
            const auto root = Root(), pack = prefs
                                                 .value(twilight ? QStringLiteral("tp_textures")
                                                                 : QStringLiteral("oot_textures"))
                                                 .toString();
            QFutureWatcher<QString> watcher;
            QEventLoop finishing;
            connect(&watcher, &QFutureWatcher<QString>::finished, &finishing, &QEventLoop::quit);
            watcher.setFuture(QtConcurrent::run([stage, root, pack, dark, twilight] {
                try {
                    DesktopImport::Finalize(stage, root, pack, dark, twilight);
                    return QString{};
                } catch (const std::exception& error) {
                    return QString::fromUtf8(error.what());
                }
            }));
            finishing.exec();
            const auto failure = watcher.result();
            if (!failure.isEmpty())
                throw std::runtime_error(failure.toStdString());
            Set(QStringLiteral("twilight_link_model"), twilight);
            Set(QStringLiteral("adult_link_model"), !dark && !twilight);
            Set(QStringLiteral("dark_link_model"), dark && !twilight);
            progress.close();
            QMessageBox::information(
                this, QStringLiteral("Mod ready"),
                QString(
                    QStringLiteral("Created %1 model variants and imported %2 adult voice clips."))
                    .arg(result.value(QStringLiteral("variants")).toInt())
                    .arg(result.value(QStringLiteral("clips")).toInt()));
            Render();
        } catch (const std::exception& error) {
            QMessageBox::warning(this, QStringLiteral("Mod setup"),
                                 QString::fromUtf8(error.what()));
        }
    }
    void Mods() {
        const auto data = Presentation();
        int model = data.value(QStringLiteral("twilight_link_model")).toBool() ? 3
                    : data.value(QStringLiteral("dark_link_model")).toBool()   ? 2
                    : data.value(QStringLiteral("adult_link_model")).toBool()  ? 1
                                                                               : 0;
        Choice(QStringLiteral("Player model"),
               {QStringLiteral("Original Link"), QStringLiteral("Adult Link"),
                QStringLiteral("Dark Link"), QStringLiteral("Twilight Princess Link")},
               model, [this](int index) {
                   Set(QStringLiteral("twilight_link_model"), index == 3);
                   Set(QStringLiteral("adult_link_model"), index == 1);
                   Set(QStringLiteral("dark_link_model"), index == 2);
                   Render();
               });
        if (model == 1) {
            Choice(QStringLiteral("Link’s tunic color"),
                   {QStringLiteral("Green (Kokiri)"), QStringLiteral("Blue (Zora)"),
                    QStringLiteral("Red (Goron)"), QStringLiteral("Purple"),
                    QStringLiteral("Black"), QStringLiteral("White"), QStringLiteral("Gold")},
                   data.value(QStringLiteral("adult_link_tunic_color")).toInt(),
                   [](int v) { Set(QStringLiteral("adult_link_tunic_color"), v); });
            Bool(QStringLiteral("Master Sword"), QStringLiteral("adult_link_master_sword"));
        }
        if (model == 3) {
            Bool(QStringLiteral("Master Sword"), QStringLiteral("twilight_link_master_sword"),
                 true);
            Note(box, QStringLiteral("TP Master Sword and sheath. Turn off to keep your MM3D "
                                     "sword. Appearance only; damage is unchanged."));
            Heading(box, QStringLiteral("Twilight Princess setup"));
            const auto prefs = Read(PrefsPath());
            Note(
                box,
                QStringLiteral("TP GameCube ROM: ") +
                    prefs.value(QStringLiteral("tp_rom")).toString(QStringLiteral("Not selected")));
            Button(box, QStringLiteral("Select your Twilight Princess ROM"), [this] {
                const auto file = QFileDialog::getOpenFileName(
                    this, QStringLiteral("Select your TP GameCube ROM"), {},
                    QStringLiteral("GameCube disc images (*.iso *.gcm *.rvz)"));
                if (!file.isEmpty()) {
                    SetPreference(QStringLiteral("tp_rom"), file);
                    Render();
                }
            })->setEnabled(!running);
            Note(box, QStringLiteral("TP textures: ") +
                          prefs.value(QStringLiteral("tp_textures"))
                              .toString(QStringLiteral("Original ROM textures")));
            Button(box, QStringLiteral("Choose your TP texture pack"), [this] {
                const auto folder = QFileDialog::getExistingDirectory(
                    this, QStringLiteral("Choose your TP texture folder"));
                if (!folder.isEmpty()) {
                    SetPreference(QStringLiteral("tp_textures"), folder);
                    Render();
                }
            })->setEnabled(!running);
            Button(box, QStringLiteral("Generate Twilight Princess Link mod"),
                   [this] { Generate(false, true); })
                ->setEnabled(!running && !GameFolder::Rom().isEmpty() &&
                             !prefs.value(QStringLiteral("tp_rom")).toString().isEmpty());
            Note(box, QStringLiteral(
                          "Includes TP action voices. Searches the selected texture folder and all "
                          "subfolders. Supports PNG and ASTC DDS replacements. Cap animation, "
                          "facial expressions, and mask fitting remain unfinished. Generate before "
                          "gameplay and restart the game after changing models."));
            return;
        }
        Note(box, QStringLiteral(
                      "Imported models include adult voices. Restart the game after changing "
                      "models. Normal MM3D abilities and transformation forms are retained."));
        Heading(box, QStringLiteral("Mod setup"));
        const auto prefs = Read(PrefsPath());
        Note(box,
             QStringLiteral("OoT3D ROM: ") +
                 prefs.value(QStringLiteral("oot_rom")).toString(QStringLiteral("Not selected")));
        auto* rom = Button(box, QStringLiteral("Select your OoT3D ROM"), [this] {
            const auto file = QFileDialog::getOpenFileName(
                this, QStringLiteral("Select your decrypted OoT3D ROM"), {},
                QStringLiteral("Game-card dumps (*.3ds *.cci)"));
            if (!file.isEmpty()) {
                SetPreference(QStringLiteral("oot_rom"), file);
                Render();
            }
        });
        rom->setEnabled(!running);
        Note(box, QStringLiteral("OoT3D textures: ") +
                      prefs.value(QStringLiteral("oot_textures"))
                          .toString(QStringLiteral("Original ROM textures")));
        auto* tex = Button(box, QStringLiteral("Choose your OoT3D texture pack"), [this] {
            const auto dir = QFileDialog::getExistingDirectory(
                this, QStringLiteral("Choose your existing OoT3D texture folder"));
            if (!dir.isEmpty()) {
                SetPreference(QStringLiteral("oot_textures"), dir);
                Render();
            }
        });
        tex->setEnabled(!running);
        Button(box, QStringLiteral("Generate Adult Link mod"), [this] { Generate(false); })
            ->setEnabled(!running && !GameFolder::Rom().isEmpty() &&
                         !prefs.value(QStringLiteral("oot_rom")).toString().isEmpty());
        Button(box, QStringLiteral("Generate Dark Link mod"), [this] { Generate(true); })
            ->setEnabled(!running && !GameFolder::Rom().isEmpty() &&
                         !prefs.value(QStringLiteral("oot_rom")).toString().isEmpty());
        Note(box, QStringLiteral("Reads your own decrypted ROMs and copies only matching "
                                 "replacement textures. Original ROMs, texture packs, and saves "
                                 "are preserved. Generation is available before gameplay."));
    }
    void Graphics() {
        Bool(QStringLiteral("Disable fog"), QStringLiteral("disable_fog"));
        const auto data = Presentation();
        const int mode = !data.value(QStringLiteral("interpolation_60fps")).toBool(true) ? 0
                         : data.value(QStringLiteral("interpolation_cpu_vertices")).toBool(true)
                             ? 1
                             : 2;
        Choice(QStringLiteral("Frame rate"),
               {QStringLiteral("Original 30 FPS"), QStringLiteral("60 FPS CPU interpolation"),
                QStringLiteral("60 FPS GPU interpolation")},
               mode, [this](int v) {
                   Set(QStringLiteral("interpolation_60fps"), v != 0);
                   Set(QStringLiteral("interpolation_cpu_vertices"), v != 2);
                   if (v != 0) {
                       QSettings ini(DesktopImport::NativeSettingsPath(), QSettings::IniFormat);
                       ini.setValue(QStringLiteral("Renderer/use_vsync"), true);
                       ini.setValue(QStringLiteral("Renderer/use_vsync/default"), false);
                       ini.setValue(QStringLiteral("Renderer/use_hw_shader"), v == 2);
                       ini.setValue(QStringLiteral("Renderer/use_hw_shader/default"), false);
                       ini.sync();
                   }
                   Render();
               });
        Number(QStringLiteral("Field of View"), QStringLiteral("game_camera_fov"), 65, 45, 70, .5);
        Number(QStringLiteral("Cutscene Field of View"), QStringLiteral("cutscene_camera_fov"), 49,
               45, 70, .5);
        Number(QStringLiteral("Camera Distance (x)"), QStringLiteral("camera_distance"), 1, .5, 3,
               .05);
        Button(box, QStringLiteral("Reset current camera"), [] {
            CameraDistance::RequestReset();
        })->setEnabled(running);
        Number(QStringLiteral("Enemy / NPC draw distance"), QStringLiteral("actor_draw_distance"),
               1, 1, 3, .05);
        Number(QStringLiteral("Vegetation / prop draw distance"),
               QStringLiteral("prop_draw_distance"), 1, 1, 3, .05);
        QSettings ini(DesktopImport::NativeSettingsPath(), QSettings::IniFormat);
        auto* n = new QDoubleSpinBox;
        n->setDecimals(0);
        n->setRange(1, 9);
        n->setValue(ini.value(QStringLiteral("Renderer/resolution_factor"), 4).toInt());
        box->addWidget(new QLabel(QStringLiteral("Internal resolution")));
        box->addWidget(n);
        connect(n, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
            if (running)
                PlayerSettings::set_resolution.store(int(v));
            else {
                QSettings out(DesktopImport::NativeSettingsPath(), QSettings::IniFormat);
                out.setValue(QStringLiteral("Renderer/resolution_factor"), int(v));
                out.setValue(QStringLiteral("Renderer/resolution_factor/default"), false);
            }
        });
        Bool(QStringLiteral("Fix bloom"), QStringLiteral("fix_bloom"), true);
        if (mode == 0)
            NativeBool(QStringLiteral("Hardware shaders"), QStringLiteral("Renderer"),
                       QStringLiteral("use_hw_shader"), false,
                       &PlayerSettings::set_hardware_shaders);
        NativeBool(QStringLiteral("VSync"), QStringLiteral("Renderer"), QStringLiteral("use_vsync"),
                   true, &PlayerSettings::set_vsync);
        NativeBool(QStringLiteral("Remember compiled shaders"), QStringLiteral("Renderer"),
                   QStringLiteral("use_disk_shader_cache"), false, &PlayerSettings::set_cache);
        NativeBool(QStringLiteral("Async shader compilation"), QStringLiteral("Renderer"),
                   QStringLiteral("async_shader_compilation"), false,
                   &PlayerSettings::set_async_shaders);
        Heading(box, QStringLiteral("Custom textures"));
        NativeBool(QStringLiteral("Use custom textures"), QStringLiteral("Utility"),
                   QStringLiteral("custom_textures"), false, &PlayerSettings::set_textures);
        Note(box, Read(PrefsPath())
                      .value(QStringLiteral("textures"))
                      .toString(QStringLiteral("Using the app texture folder")));
        Button(box, QStringLiteral("Choose texture folder"), [this] {
            const auto dir = QFileDialog::getExistingDirectory(
                this, QStringLiteral("Choose your existing MM3D texture folder"));
            if (!dir.isEmpty()) {
                SetPreference(QStringLiteral("textures"), dir);
                Render();
            }
        });
        Button(box, QStringLiteral("Use app texture folder"), [this] {
            SetPreference(QStringLiteral("textures"), {});
            Render();
        });
        Note(box, QStringLiteral("Use an existing Azahar texture pack directly. Folder changes "
                                 "apply on the next game launch."));
    }
    void Hud() {
        Number(QStringLiteral("Overall HUD scale"), QStringLiteral("player_hud_scale"), 0, -10, 10);
        Number(QStringLiteral("Overall HUD opacity"), QStringLiteral("player_hud_opacity"), 1, 0, 1,
               .05);
        Number(QStringLiteral("Top padding"), QStringLiteral("player_hud_top_padding"), 0, 0, 40);
        Number(QStringLiteral("Bottom padding"), QStringLiteral("player_hud_bottom_padding"), 0, 0,
               40);
        Number(QStringLiteral("Location banner Y"), QStringLiteral("player_hud_banner_y"), 0, -240,
               240);
        Number(QStringLiteral("Location banner darkness"),
               QStringLiteral("player_hud_banner_darkness"), 0, 0, 1, .05);
        const QStringList keys{QStringLiteral("dpad"),    QStringLiteral("magic"),
                               QStringLiteral("hearts"),  QStringLiteral("buttons"),
                               QStringLiteral("minimap"), QStringLiteral("clock"),
                               QStringLiteral("rupees"),  QStringLiteral("minigame"),
                               QStringLiteral("carrots"), QStringLiteral("timer")},
            labels{QStringLiteral("D-pad"),         QStringLiteral("Magic meter"),
                   QStringLiteral("Hearts"),        QStringLiteral("Buttons"),
                   QStringLiteral("Minimap"),       QStringLiteral("Clock"),
                   QStringLiteral("Rupees"),        QStringLiteral("Minigame counter"),
                   QStringLiteral("Epona carrots"), QStringLiteral("Timer")};
        for (int i = 0; i < keys.size(); ++i) {
            auto* group = new QGroupBox(labels[i]);
            auto* rows = new QVBoxLayout(group);
            auto* outer = box;
            box = rows;
            Number(QStringLiteral("Scale"),
                   QStringLiteral("player_hud_") + keys[i] + QStringLiteral("_scale"), 0, -10, 10);
            Number(QStringLiteral("X position"),
                   QStringLiteral("player_hud_") + keys[i] + QStringLiteral("_x"), 0, -400, 400);
            Number(QStringLiteral("Y position"),
                   QStringLiteral("player_hud_") + keys[i] + QStringLiteral("_y"), 0, -240, 240);
            if (keys[i] == QStringLiteral("hearts"))
                Number(QStringLiteral("Heart spacing"), QStringLiteral("player_hud_heart_spacing"),
                       0, -40, 40);
            if (keys[i] == QStringLiteral("magic"))
                Number(QStringLiteral("Height"), QStringLiteral("player_hud_magic_height"), 0, -10,
                       10);
            if (keys[i] == QStringLiteral("minimap"))
                Number(QStringLiteral("Opacity"), QStringLiteral("minimap_opacity"), .85, 0, 1,
                       .05);
            if (keys[i] == QStringLiteral("buttons"))
                for (const auto& button :
                     QStringList{QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("i"),
                                 QStringLiteral("ii"), QStringLiteral("a"), QStringLiteral("b")}) {
                    Number(button.toUpper() + QStringLiteral(" X"),
                           QStringLiteral("player_hud_button_") + button + QStringLiteral("_x"), 0,
                           -400, 400);
                    Number(button.toUpper() + QStringLiteral(" Y"),
                           QStringLiteral("player_hud_button_") + button + QStringLiteral("_y"), 0,
                           -240, 240);
                }
            box = outer;
            box->addWidget(group);
        }
    }
    void Render() {
        auto* content = new QWidget;
        content->setObjectName(QStringLiteral("Panel"));
        auto* columns = new QHBoxLayout(content);
        columns->setContentsMargins(12, 12, 12, 12);
        auto* form = new QWidget(content);
        form->setMaximumWidth(fontMetrics().height() * 24);
        form->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        columns->addWidget(form, 1, Qt::AlignTop);
        columns->addStretch(0);
        box = new QVBoxLayout(form);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(14);
        Heading(box, page);
        if (page == QStringLiteral("General")) {
            Bool(QStringLiteral("Show frame rate"), QStringLiteral("show_performance"));
            Note(box, QStringLiteral("MM3D ROM: ") + GameFolder::Rom());
            Button(box, QStringLiteral("Choose MM3D ROM"), [this] {
                const auto file = QFileDialog::getOpenFileName(
                    this, QStringLiteral("Select your decrypted MM3D USA Rev 1 ROM"), {},
                    QStringLiteral("Game-card dumps (*.3ds *.cci)"));
                if (!file.isEmpty()) {
                    SetPreference(QStringLiteral("mm_rom"), file);
                    Render();
                }
            })->setEnabled(!running);
            Number(QStringLiteral("Menu size"), QStringLiteral("desktop_menu_text_size"), 5, 0, 40);
            Note(box,
                 QStringLiteral(
                     "Settings save as you change them and are shared with the in-game menu."));
        } else if (page == QStringLiteral("Graphics"))
            Graphics();
        else if (page == QStringLiteral("HUD"))
            Hud();
        else if (page == QStringLiteral("Touch")) {
            NativeBool(QStringLiteral("Swap screens"), QStringLiteral("Layout"),
                       QStringLiteral("swap_screen"), false);
            Note(box, QStringLiteral("Shows the game’s touchscreen. Use your mouse to interact "
                                     "with it, then turn Swap screens off to return to gameplay."));
        } else if (page == QStringLiteral("Mods"))
            Mods();
        else if (page == QStringLiteral("Controls")) {
            Bool(QStringLiteral("Hide item menu button prompts"),
                 QStringLiteral("player_hud_hide_menu_prompts"));
            Bool(QStringLiteral("Single-screen game instructions"),
                 QStringLiteral("single_screen_text"), true);
            Bool(QStringLiteral("Controller neutral item prompts"),
                 QStringLiteral("controller_neutral_item_prompts"), true);
            DesktopImport::BindingControls(box, this);
        } else if (page == QStringLiteral("Audio")) {
            Bool(QStringLiteral("Accurate HLE audio"), QStringLiteral("accurate_hle_audio"), true);
            Bool(QStringLiteral("Mute music"), QStringLiteral("mute_music"));
            DesktopImport::VolumeControl(box, running);
        } else if (page == QStringLiteral("Difficulty")) {
            Number(QStringLiteral("Damage taken"), QStringLiteral("damage_multiplier"), 1, 1, 16);
            Number(QStringLiteral("Red potion healing"), QStringLiteral("red_potion_hearts"), 20, 1,
                   20);
            Number(QStringLiteral("Blue potion healing"), QStringLiteral("blue_potion_hearts"), 20,
                   1, 20);
            Number(QStringLiteral("Bottled fairy healing"), QStringLiteral("bottled_fairy_hearts"),
                   10, 1, 20);
            for (const auto& item : std::initializer_list<std::pair<QString, QString>>{
                     {QStringLiteral("Weaker enhanced defense"),
                      QStringLiteral("weaker_enhanced_defense")},
                     {QStringLiteral("Disable heart drops"), QStringLiteral("disable_heart_drops")},
                     {QStringLiteral("Disable rupee drops"), QStringLiteral("disable_rupee_drops")},
                     {QStringLiteral("Disable fairy drops"), QStringLiteral("disable_fairy_drops")},
                     {QStringLiteral("Disable magic drops"), QStringLiteral("disable_magic_drops")},
                     {QStringLiteral("Disable ammo pickups"),
                      QStringLiteral("disable_ammo_pickups")},
                     {QStringLiteral("Lose rupees on death"),
                      QStringLiteral("lose_rupees_on_death")},
                     {QStringLiteral("World ends on death"), QStringLiteral("world_ends_on_death")},
                     {QStringLiteral("Dying moves time forward 6 hours"),
                      QStringLiteral("advance_six_hours_on_death")},
                     {QStringLiteral("Fast time"), QStringLiteral("fast_time")},
                     {QStringLiteral("Increase purchase prices and fees"),
                      QStringLiteral("double_prices")}})
                Bool(item.first, item.second);
            Number(QStringLiteral("Price multiplier"), QStringLiteral("price_multiplier"), 2, 2, 3);
        } else if (page == QStringLiteral("Restoration")) {
            Bool(QStringLiteral("Bombers don’t approach automatically"),
                 QStringLiteral("disable_bomber_approach"));
            Bool(QStringLiteral("Activate owl statues with a sword"),
                 QStringLiteral("sword_activate_owls"));
            Bool(QStringLiteral("Classic Song of Double Time"),
                 QStringLiteral("classic_double_time"));
            Bool(QStringLiteral("Classic saving"), QStringLiteral("classic_saving"));
            Note(box, QStringLiteral("These options change gameplay and saving rules."));
        } else if (page == QStringLiteral("Quality of life")) {
            Number(QStringLiteral("Walk speed (x)"), QStringLiteral("walk_speed"), 1, .5, 3, .25);
            Number(QStringLiteral("Climb speed (x)"), QStringLiteral("climb_speed"), 1, .5, 3, .25);
            Number(QStringLiteral("Swim speed (x)"), QStringLiteral("swim_speed"), 1, .5, 3, .25);
            Button(box, QStringLiteral("Reset movement speeds"), [this] {
                for (const auto& key : {QStringLiteral("walk_speed"), QStringLiteral("climb_speed"),
                                        QStringLiteral("swim_speed")})
                    Set(QString(key), 1);
                Render();
            });
            Bool(QStringLiteral("Disable low-heart beeping"),
                 QStringLiteral("disable_low_health_beep"));
            Bool(QStringLiteral("Disable Tatl reminders"),
                 QStringLiteral("disable_tatl_reminders"));
        } else if (page == QStringLiteral("Cheats"))
            Note(box,
                 QStringLiteral(
                     "Cheats require a loaded save. During gameplay, use the Cheats page in this "
                     "same settings menu; availability is checked against the running game."));
        else if (page == QStringLiteral("About"))
            Note(
                box,
                QStringLiteral(
                    "Majora ReMasked uses DekUI, Azahar, and Project Restoration. This is an "
                    "emulator-based fork, not a recompilation. See bundled credits and licenses."));
        box->addStretch();
        // A setting can rebuild its own page from a widget signal. setWidget()
        // deletes the previous page immediately, invalidating the emitting
        // combo box while Qt is still closing its popup. Retire it only once
        // the current event has finished.
        if (auto* previous = scroll->takeWidget()) {
            previous->hide();
            previous->deleteLater();
        }
        scroll->setWidget(content);
    }

public:
    SettingsDialog(QWidget* parent, bool active, bool open_mods = false)
        : QDialog(parent), running(active) {
        setProperty("dekuiMenu", true);
        setWindowTitle(QStringLiteral("Majora ReMasked / Settings"));
        const int text_size =
            16 + int(Presentation().value(QStringLiteral("desktop_menu_text_size")).toInt(5) * .3);
        setStyleSheet(QString::fromUtf8(Theme) +
                      QStringLiteral(" QWidget{font-size:%1px;} QLabel#Heading{font-size:%2px;}")
                          .arg(text_size)
                          .arg(text_size + 4));
        resize(parent ? parent->size() : QSize(1100, 720));
        auto* root = new QVBoxLayout(this);
        auto* header = new QHBoxLayout;
        auto* title = new QLabel(QStringLiteral("Majora ReMasked / Settings"));
        title->setObjectName(QStringLiteral("Heading"));
        header->addWidget(title, 1);
        auto* back = new QPushButton(QStringLiteral("Back"));
        header->addWidget(back);
        root->addLayout(header);
        connect(back, &QPushButton::clicked, this, &QDialog::accept);
        auto* body = new QHBoxLayout;
        rail = new QListWidget;
        for (const auto& name : QStringList{
                 QStringLiteral("General"), QStringLiteral("Controls"), QStringLiteral("Touch"),
                 QStringLiteral("Graphics"), QStringLiteral("Audio"), QStringLiteral("HUD"),
                 QStringLiteral("Difficulty"), QStringLiteral("Restoration"),
                 QStringLiteral("Quality of life"), QStringLiteral("Mods"),
                 QStringLiteral("Cheats"), QStringLiteral("About")})
            rail->addItem(name);
        body->addWidget(rail, 1);
        scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        body->addWidget(scroll, 3);
        root->addLayout(body);
        connect(rail, &QListWidget::currentTextChanged, this, [this](const QString& name) {
            page = name;
            Render();
        });
        rail->setCurrentRow(open_mods ? 9 : 0);
    }
};
class Backdrop final : public QDialog {
    QPixmap image;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(QStringLiteral("#08070d")));
        if (!image.isNull()) {
            const auto scaled = image.scaled(QSize(qRound(width() * .56), qRound(height() * .78)),
                                             Qt::KeepAspectRatio, Qt::SmoothTransformation);
            p.drawPixmap(qRound(width() * .30 - scaled.width() / 2.0),
                         qRound(height() * .55 - scaled.height() / 2.0), scaled);
        }
        auto font = p.font();
        font.setPixelSize(14);
        p.setFont(font);
        p.setPen(QColor(QStringLiteral("#9993a5")));
        p.drawText(QRect(28, height() - 36, width() - 56, 24), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("MAJORA REMASKED  ·  v%1").arg(QString::fromUtf8(Common::g_build_version)));
    }

public:
    Backdrop(QWidget* parent, bool open_mods)
        : QDialog(parent), image(QStringLiteral(":/dekui/startup.png")) {
        setProperty("dekuiMenu", true);
        setObjectName(QStringLiteral("Startup"));
        setWindowTitle(QStringLiteral("Majora ReMasked"));
        setStyleSheet(QString::fromUtf8(Theme));
        resize(1200, 720);
        auto* all = new QHBoxLayout(this);
        all->setContentsMargins(36, 30, 40, 32);
        all->addStretch(3);
        auto* menu = new QVBoxLayout;
        auto* brand = new QLabel(QStringLiteral("A DekUI Fork"));
        brand->setAlignment(Qt::AlignRight);
        brand->setStyleSheet(QStringLiteral("color:#b97df2;font-style:italic;font-size:24px;"));
        auto* title = new QLabel(QStringLiteral("Majora ReMasked"));
        title->setAlignment(Qt::AlignRight);
        title->setStyleSheet(QStringLiteral("font-weight:bold;font-size:44px;"));
        menu->addWidget(title);
        menu->addWidget(brand);
        menu->addStretch();
        auto* start =
            Button(menu,
                   GameFolder::Rom().isEmpty() ? QStringLiteral("Select ROM")
                                               : QStringLiteral("Start game"),
                   [this] {
                       if (GameFolder::Rom().isEmpty()) {
                           const auto file = QFileDialog::getOpenFileName(
                               this, QStringLiteral("Select your decrypted MM3D USA Rev 1 ROM"), {},
                               QStringLiteral("Game-card dumps (*.3ds *.cci)"));
                           if (file.isEmpty())
                               return;
                           SetPreference(QStringLiteral("mm_rom"), file);
                       }
                       accept();
                   });
        start->setStyleSheet(
            QStringLiteral("text-align:right;font-size:28px;background:transparent;padding:14px;"));
        Button(menu, QStringLiteral("Settings"), [this] { SettingsDialog(this, false).exec(); });
        Button(menu, QStringLiteral("About"), [this] {
            QMessageBox::information(
                this, QStringLiteral("Majora ReMasked"),
                QStringLiteral(
                    "Majora ReMasked\nA DekUI Fork\n\nMajora’s Mask 3D\nBuilt on DekUI, Azahar and Project "
                    "Restoration.\nYour game and optional OoT3D mod inputs are supplied by you."));
        });
        Button(menu, QStringLiteral("Quit"), [this] { reject(); });
        for (int i = 0; i < menu->count(); ++i)
            if (auto* button = qobject_cast<QPushButton*>(menu->itemAt(i)->widget())) {
                button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
                menu->setAlignment(button, Qt::AlignRight);
                button->setMinimumWidth(0);
                button->setStyleSheet(QStringLiteral(
                    "QPushButton{text-align:right;font-size:26px;background:transparent;padding:"
                    "12px 6px;border:0;} "
                    "QPushButton:hover,QPushButton:focus{color:#d9b5ff;background:#291c38;}"));
            }
        menu->addSpacing(20);
        all->addLayout(menu, 2);
        auto* settings = new QShortcut(QKeySequence::Preferences, this);
        connect(settings, &QShortcut::activated, this,
                [this] { SettingsDialog(this, false).exec(); });
        start->setFocus();
        if (open_mods)
            QTimer::singleShot(0, this, [this] { SettingsDialog(this, false, true).exec(); });
    }
};
} // namespace
bool Startup(QWidget* parent, bool open_mods) {
    QFontDatabase::addApplicationFont(QStringLiteral(":/dekui/font.ttf"));
    DesktopImport::EnsureController();
    MenuInput input(qApp);
    return Backdrop(parent, open_mods).exec() == QDialog::Accepted;
}
void RuntimeInput() {
    if (!presentation_available.load() || QApplication::activeModalWidget())
        return;
    static std::array<std::unique_ptr<Input::ButtonDevice>, 10> devices;
    static std::string signature;
    static std::array<bool, 10> previous{};
    const int logical[]{Settings::NativeButton::Up,    Settings::NativeButton::Down,
                        Settings::NativeButton::Left,  Settings::NativeButton::Right,
                        Settings::NativeButton::A,     Settings::NativeButton::ZL,
                        Settings::NativeButton::ZR,    Settings::NativeButton::B,
                        Settings::NativeButton::Start, Settings::NativeButton::Select};
    const auto& bindings = Settings::values.current_input_profile.buttons;
    std::string current;
    for (int i : logical)
        current += bindings[i] + "|";
    if (signature != current) {
        signature = current;
        for (int i = 0; i < 10; ++i)
            devices[i] = Input::CreateDevice<Input::ButtonDevice>(bindings[logical[i]]);
        previous.fill(true);
    }
    std::array<bool, 10> down{};
    for (int i = 0; i < 10; ++i)
        down[i] = devices[i] && devices[i]->GetStatus();
    if (down[8] && down[9] && !(previous[8] && previous[9]))
        ToggleOverlayView(true);
    if (overlay_visible.load()) {
        for (int i = 0; i < 7; ++i)
            if (down[i] != previous[i])
                AndroidMenuKey(i, down[i]);
        if (down[7] && !previous[7])
            ToggleOverlay();
    }
    previous = down;
}
void Settings(QWidget* parent, bool running) {
    SettingsDialog(parent, running).exec();
}
} // namespace MM3D::DesktopFrontend
