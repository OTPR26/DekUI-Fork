// SPDX-License-Identifier: GPL-2.0-or-later
#include <filesystem>
#include <iostream>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include "citra_qt/configuration/config.h"
#include "citra_qt/dekui_bindings.h"
#include "citra_qt/dekui_import.h"
#include "citra_qt/uisettings.h"
#include "common/cityhash.h"
#include "common/file_util.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "mm3d/desktop_storage.h"

namespace {
void Check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
void Write(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    Check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "fixture write");
}
void Stage(const QString& root, const QString& name) {
    const QString stage = root + QStringLiteral("/user/load/mm3d-models/") + name;
    QDir().mkpath(stage + QStringLiteral("/texture-sources"));
    Write(stage + QStringLiteral("/texture-sources/pixels.bin"), QByteArray(128, '\x7f'));
    Write(stage + QStringLiteral("/texture-sources/manifest.json"),
          QJsonDocument(QJsonArray{QJsonObject{
                            {QStringLiteral("file"), QStringLiteral("texture-sources/pixels.bin")},
                            {QStringLiteral("width"), 8},
                            {QStringLiteral("height"), 8},
                            {QStringLiteral("format"), 3},
                            {QStringLiteral("icon"), false}}})
              .toJson());
    Write(stage + QStringLiteral("/generated.json"), "{}");
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (argc == 4 || argc == 5) {
            MM3D::DesktopImport::Finalize(QString::fromLocal8Bit(argv[1]),
                                          QString::fromLocal8Bit(argv[2]),
                                          QString::fromLocal8Bit(argv[3]), argc == 4, argc == 5);
            std::cout << "Private Dark Link import finalized\n";
            return 0;
        }
        QTemporaryDir temp;
        Check(temp.isValid(), "temporary directory");
        const QString root = temp.path();
#ifdef Q_OS_MACOS
        // Application Support migration publishes only verified copies, keeps
        // original saves, preserves external texture links and is idempotent.
        {
            const auto portable = root + QStringLiteral("/portable"),
                       support = root + QStringLiteral("/support/DekUI Fork");
            Write(portable + QStringLiteral("/user/sdmc/save.bin"), "precious-save");
            Write(portable + QStringLiteral("/user/config/qt-config.ini"),
                  ("[Paths]\nowned=" + portable + "/user/cache\nexternal=" + root + "/textures\n")
                      .toUtf8());
            Write(portable + QStringLiteral("/desktop-settings.json"),
                  "{\"mm_rom\":\"/external/game.3ds\"}");
            QDir().mkpath(root + QStringLiteral("/textures"));
            QDir().mkpath(portable + QStringLiteral("/user/load"));
            std::filesystem::create_directory_symlink(
                std::filesystem::path((root + QStringLiteral("/textures")).toStdString()),
                std::filesystem::path(
                    (portable + QStringLiteral("/user/load/textures")).toStdString()));
            Check(MM3D::DesktopStorage::Migrate(portable, support).isEmpty(),
                  "storage migration failed");
            Check(MM3D::DesktopStorage::Digest(portable + "/user/sdmc/save.bin") ==
                      MM3D::DesktopStorage::Digest(support + "/user/sdmc/save.bin"),
                  "save copy changed");
            Check(QFileInfo(support + "/user/load/textures").isSymLink(),
                  "external pack was copied instead of linked");
            QFile settings(support + "/user/config/qt-config.ini");
            Check(settings.open(QIODevice::ReadOnly), "migrated config open");
            const auto bytes = settings.readAll();
            Check(bytes.contains((support + "/user/cache").toUtf8()) &&
                      bytes.contains((root + "/textures").toUtf8()),
                  "path rebasing changed external location");
            Write(support + "/user/sdmc/save.bin", "newer-progress");
            Check(MM3D::DesktopStorage::Migrate(portable, support).isEmpty(),
                  "repeat migration failed");
            Check(MM3D::DesktopStorage::Digest(portable + "/user/sdmc/save.bin") !=
                      MM3D::DesktopStorage::Digest(support + "/user/sdmc/save.bin"),
                  "repeat migration overwrote newer save");
            const auto conflict = root + "/conflict";
            Write(conflict + "/user/save.bin", "other-progress");
            Check(!MM3D::DesktopStorage::Migrate(portable, conflict).isEmpty(),
                  "existing destination accepted");
            const auto broken = root + "/broken";
            QDir().mkpath(broken);
            Write(broken + "/user", "not-a-directory");
            Check(!MM3D::DesktopStorage::Migrate(broken, root + "/invalid-destination").isEmpty(),
                  "invalid user tree accepted");
            Check(!QFileInfo::exists(root + "/invalid-destination"),
                  "failed migration published partial data");
        }
#endif
        FileUtil::SetUserPath((root + QStringLiteral("/config-test/")).toStdString());
        {
            QtConfig config("binding-regression");
            const auto count = Settings::values.input_profiles.size();
            const auto maps = Settings::values.touch_from_button_maps.size();
            const auto dirs = UISettings::values.game_dirs.size();
            const auto shortcuts = UISettings::values.shortcuts.size();
            QSettings saved(
                QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir)) +
                    QStringLiteral("binding-regression.ini"),
                QSettings::IniFormat);
            // Reproduce stale array data left by earlier versions and a saved favorite.
            saved.setValue(QStringLiteral("Controls/profiles/999/name"), QStringLiteral("stale"));
            saved.setValue(QStringLiteral("UI/Paths/gamedirs/999/path"), QStringLiteral("stale"));
            saved.setValue(QStringLiteral("UI/GameList/favorites/size"), 1);
            saved.setValue(QStringLiteral("UI/GameList/favorites/1/program_id"),
                           qulonglong{0x125500});
            for (int button : {0, 1, 0, 1}) {
                const auto key = QStringLiteral("Controls/profiles/1/button_a");
                const auto binding =
                    QStringLiteral("engine:sdl,api:controller,guid:test,port:0,button:%1")
                        .arg(button);
                saved.setValue(key, binding);
                saved.setValue(key + QStringLiteral("/default"), false);
                saved.sync();
                config.Reload();
                Check(UISettings::values.game_dirs.size() == dirs,
                      "reload duplicated game directories");
                Check(UISettings::values.shortcuts.size() == shortcuts,
                      "reload duplicated shortcuts");
                Check(UISettings::values.favorited_ids.size() == 1, "reload duplicated favorites");
                Check(Settings::values.input_profiles.size() == count,
                      "reload duplicated profiles");
                Check(Settings::values.touch_from_button_maps.size() == maps,
                      "reload duplicated touch maps");
                Check(Settings::values.current_input_profile.buttons[Settings::NativeButton::A] ==
                          binding.toStdString(),
                      "reload ignored remapped confirm button");
            }
            // Four successive in-game captures, each followed by the emulator's
            // normal save/reload cycle. The fourth must not revert the first three.
            saved.setValue(QStringLiteral("Controls/profiles/1/name"),
                           QStringLiteral("Controller (automatic)"));
            saved.setValue(QStringLiteral("Controls/profiles/1/name/default"), false);
            saved.sync();
            config.Reload();
            const auto bindings_path = saved.fileName();
            const int faces[]{Settings::NativeButton::A, Settings::NativeButton::B,
                              Settings::NativeButton::X, Settings::NativeButton::Y};
            for (int i = 0; i < 4; ++i) {
                config.Save();
                const auto bind =
                    QStringLiteral("engine:sdl,api:controller,guid:test,port:0,button:%1").arg(i);
                Check(MM3D::ControllerBindings::Save(bindings_path, faces[i], bind),
                      "manual binding commit");
                // Also exercise a settings save before reloading: the live and
                // stored copies must already agree at this point.
                config.Save();
                config.Reload();
                for (int j = 0; j <= i; ++j)
                    Check(Common::ParamPackage(
                              Settings::values.current_input_profile.buttons[faces[j]])
                                  .Get("button", -1) == j,
                          "later capture reverted earlier face binding");
                Check(Settings::values.current_input_profile.name == "Controller (custom)",
                      "manual profile remained automatic");
            }
            const auto kept_buttons = Settings::values.current_input_profile.buttons;
            Check(MM3D::ControllerBindings::Save(
                      bindings_path, 1, QStringLiteral("engine:sdl,axis_x:2,axis_y:3"), true),
                  "manual stick commit");
            config.Save();
            config.Reload();
            Check(Settings::values.current_input_profile.buttons == kept_buttons,
                  "stick edit changed face mappings");
            saved.sync();
            Check(!saved.contains(QStringLiteral("Controls/profiles/999/name")),
                  "save retained stale profile keys");
            Check(!saved.contains(QStringLiteral("UI/Paths/gamedirs/999/path")),
                  "save retained stale directory keys");
        }

        const QString old = root + QStringLiteral("/user/load/mm3d-models/dark-link/previous.txt"),
                      save = root + QStringLiteral("/user/save/sentinel");
        Write(old, "old-model");
        Write(save, "save-data");
        const QByteArray raw(128, '\x7f');
        const QString hash = QString::number(Common::CityHash64(raw.constData(), raw.size()), 16)
                                 .rightJustified(16, QLatin1Char('0'))
                                 .toUpper();
        const QString pack = root + QStringLiteral("/pack");
        QDir().mkpath(pack);
        QImage image(8, 8, QImage::Format_RGBA8888);
        image.fill(Qt::red);
        Check(
            image.save(pack + QStringLiteral("/tex1_8x8_") + hash + QStringLiteral("_3_mip0.png")),
            "png fixture");
        // The ordinary pack may be on another volume. Never publish into its symlink.
        const QString external = root + QStringLiteral("/external-pack");
        Write(external + QStringLiteral("/sentinel"), "external-pack");
        QDir().mkpath(root + QStringLiteral("/user/load/textures"));
#ifndef Q_OS_WIN
        // QFile::link creates a shortcut on Windows. Use a native directory
        // symlink so the cross-platform fixture exercises the actual pack path.
        std::filesystem::create_directory_symlink(
            std::filesystem::path(external.toStdString()),
            std::filesystem::path(
                (root + QStringLiteral("/user/load/textures/0004000000125500")).toStdString()));
#else
        QDir().mkpath(root + QStringLiteral("/user/load/textures/0004000000125500"));
#endif
        Stage(root, QStringLiteral(".dark-link-test"));
        MM3D::DesktopImport::Finalize(
            root + QStringLiteral("/user/load/mm3d-models/.dark-link-test"), root, pack, true);
        Check(QFileInfo::exists(root +
                                QStringLiteral("/user/load/mm3d-model-textures/0004000000125500/"
                                               "dark-link-generated/tex1_8x8_") +
                                hash + QStringLiteral("_3_mip0.png")),
              "matching HD texture missing");
        Check(QFileInfo::exists(save), "save replaced");
        Check(QDir(external).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot) ==
                  QStringList{QStringLiteral("sentinel")},
              "external pack modified");
        Check(!QFileInfo::exists(old), "new model did not publish");
        const auto backups = QDir(root + QStringLiteral("/user/load/mm3d-models"))
                                 .entryList({QStringLiteral("dark-link-backup-*")}, QDir::Dirs);
        Check(backups.size() == 1, "previous model backup missing");
        Check(QFileInfo::exists(root + QStringLiteral("/user/load/mm3d-models/") + backups.front() +
                                QStringLiteral("/previous.txt")),
              "backup lost old model");
        QSettings ini(root + QStringLiteral("/user/config/qt-config.ini"), QSettings::IniFormat);
        Check(!ini.value(QStringLiteral("Renderer/use_disk_shader_cache"), true).toBool(),
              "shader cache not disabled");
        Check(ini.value(QStringLiteral("Utility/custom_textures"), false).toBool(),
              "custom textures not enabled");
        Stage(root, QStringLiteral(".dark-link-invalid"));
        Write(root + QStringLiteral(
                         "/user/load/mm3d-models/.dark-link-invalid/texture-sources/pixels.bin"),
              "short");
        bool rejected = false;
        try {
            MM3D::DesktopImport::Finalize(
                root + QStringLiteral("/user/load/mm3d-models/.dark-link-invalid"), root, pack,
                true);
        } catch (const std::exception&) {
            rejected = true;
        }
        Check(rejected, "damaged texture accepted");
        Check(QFileInfo::exists(root +
                                QStringLiteral("/user/load/mm3d-models/dark-link/generated.json")),
              "failed generation damaged installed model");
        Check(QFileInfo::exists(save), "failed generation damaged save");
        // TP packs can nest files arbitrarily and include several resolutions.
        const auto tpStage = root + QStringLiteral("/user/load/mm3d-models/.twilight-link-test");
        for (const auto& variant : {QStringLiteral("kokiri"), QStringLiteral("master")})
            Write(tpStage + QStringLiteral("/variants/") + variant +
                      QStringLiteral("/romfs/actors/zelda2_link_child_new.gar.lzs"),
                  "model-fixture");
        Write(tpStage + QStringLiteral("/voices/romfs/sound/sequence/JokerSound.bcsar"),
              "voice-fixture");
        Write(tpStage + QStringLiteral("/generated.json"), "{}");
        Write(tpStage + QStringLiteral("/build/pixels.bin"), QByteArray(64, '\0'));
        Write(
            tpStage + QStringLiteral("/texture-manifest.json"),
            QJsonDocument(QJsonArray{QJsonObject{
                              {QStringLiteral("file"), QStringLiteral("build/pixels.bin")},
                              {QStringLiteral("width"), 8},
                              {QStringLiteral("height"), 8},
                              {QStringLiteral("source_hash"), QStringLiteral("0123456789ABCDEF")}}})
                .toJson());
        QDir().mkpath(pack + QStringLiteral("/Characters/Link"));
        QImage small(16, 16, QImage::Format_RGBA8888);
        small.fill(Qt::red);
        QImage large(32, 32, QImage::Format_RGBA8888);
        large.fill(Qt::blue);
        large.setPixelColor(0, 31, Qt::green);
        Check(small.save(pack + QStringLiteral("/tex1_8x8_0123456789abcdef_14.png")),
              "small TP fixture");
        Check(
            large.save(pack + QStringLiteral("/Characters/Link/tex1_8x8_0123456789abcdef_14.png")),
            "large TP fixture");
        MM3D::DesktopImport::Finalize(tpStage, root, pack, false, true);
        const auto tpTextures =
            root + QStringLiteral(
                       "/user/load/mm3d-model-textures/0004000000125500/twilight-link-generated");
        const auto files = QDir(tpTextures).entryList({QStringLiteral("*.png")}, QDir::Files);
        Check(!files.empty(), "nested TP replacement was not installed");
        const QImage imported(tpTextures + QStringLiteral("/") + files.front());
        Check(imported.size() == QSize(32, 32) && imported.pixelColor(0, 0) == QColor(Qt::green) &&
                  imported.pixelColor(0, 31) == QColor(Qt::blue),
              "TP duplicate selection did not choose highest resolution");
        Check(QFileInfo::exists(save), "TP generation damaged save");
        Check(QFileInfo::exists(root +
                                QStringLiteral("/user/load/mm3d-models/dark-link/generated.json")),
              "TP generation damaged another mod");
        std::cout << "Desktop texture import, backup, settings and failure preservation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
