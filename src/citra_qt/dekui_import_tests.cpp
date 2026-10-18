// SPDX-License-Identifier: GPL-2.0-or-later
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
#include "citra_qt/dekui_import.h"
#include "common/cityhash.h"

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
        if (argc == 4) {
            MM3D::DesktopImport::Finalize(QString::fromLocal8Bit(argv[1]),
                                          QString::fromLocal8Bit(argv[2]),
                                          QString::fromLocal8Bit(argv[3]), true);
            std::cout << "Private Dark Link import finalized\n";
            return 0;
        }
        QTemporaryDir temp;
        Check(temp.isValid(), "temporary directory");
        const QString root = temp.path();
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
        Stage(root, QStringLiteral(".dark-link-test"));
        MM3D::DesktopImport::Finalize(
            root + QStringLiteral("/user/load/mm3d-models/.dark-link-test"), root, pack, true);
        Check(QFileInfo::exists(
                  root +
                  QStringLiteral(
                      "/user/load/textures/0004000000125500/dark-link-generated/tex1_8x8_") +
                  hash + QStringLiteral("_3_mip0.png")),
              "matching HD texture missing");
        Check(QFileInfo::exists(save), "save replaced");
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
        std::cout << "Desktop texture import, backup, settings and failure preservation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
