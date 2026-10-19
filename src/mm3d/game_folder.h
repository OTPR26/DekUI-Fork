// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <QStandardPaths>
#include "mm3d/desktop_storage.h"

namespace MM3D::GameFolder {
inline constexpr auto Hash = "9c756279e3ec00bb9009f92ca2e7b9ec8b1fcdfcd9de057658b209caa24323da";
inline QString LegacyRoot() {
    // AppImage mounts its executable read-only; use the folder holding the image.
    const auto appimage = qEnvironmentVariable("APPIMAGE");
#ifdef Q_OS_MACOS
    // The executable is inside DekUI Fork.app/Contents/MacOS; portable data and
    // the user's game belong beside the app, outside its signed contents.
    QDir bundle(QCoreApplication::applicationDirPath());
    if (bundle.dirName() == QStringLiteral("MacOS") && bundle.cdUp() &&
        bundle.dirName() == QStringLiteral("Contents") && bundle.cdUp() &&
        bundle.dirName().endsWith(QStringLiteral(".app")) && bundle.cdUp())
        return bundle.absolutePath();
#endif
    return appimage.isEmpty() ? QCoreApplication::applicationDirPath()
                              : QFileInfo(appimage).absolutePath();
}
inline QString PackageRoot() {
#ifdef Q_OS_MACOS
    const auto embedded=QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../Resources"));
    if (QFileInfo::exists(embedded+QStringLiteral("/tools/mm3d/graphical_setup.py"))) return embedded;
#endif
    return LegacyRoot();
}
inline QString Root() {
#ifdef Q_OS_MACOS
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)+QStringLiteral("/DekUI Fork");
#else
    return LegacyRoot();
#endif
}
inline QString InitializeStorage() {
#ifdef Q_OS_MACOS
    return DesktopStorage::Migrate(LegacyRoot(),Root());
#else
    return {};
#endif
}
inline QString FindRom(const QString& root) {
    QFile selection(root+QStringLiteral("/desktop-settings.json"));
    if(selection.open(QIODevice::ReadOnly)) {
        const auto chosen=QJsonDocument::fromJson(selection.readAll()).object().value(QStringLiteral("mm_rom")).toString();
        if(!chosen.isEmpty())return QFileInfo(chosen).isFile()?chosen:QString{};
    }
    QStringList candidates;
    for (const auto& file : QDir(root).entryInfoList(QDir::Files, QDir::Name)) {
        const auto suffix=file.suffix().toLower();
        if(suffix==QStringLiteral("cci") || suffix==QStringLiteral("3ds"))
            candidates.push_back(file.absoluteFilePath());
    }
    if(candidates.size()==1)return candidates.front();
    if(!candidates.empty())return {}; // Never silently choose between games.
    // Preserve existing portable installations without moving their game/saves.
    const auto legacy=root+QStringLiteral("/game/mm3d.cci");
    return QFileInfo(legacy).isFile()?legacy:QString{};
}
inline QString Rom() {
    const auto chosen=FindRom(Root());
#ifdef Q_OS_MACOS
    if(chosen.isEmpty()&&!QFileInfo::exists(Root()+QStringLiteral("/desktop-settings.json")))
        return FindRom(LegacyRoot());
#endif
    return chosen;
}
inline QString MissingGameMessage() {
    return QStringLiteral("Select your decrypted MM3D USA Rev 1 game-card dump (.cci or .3ds) from the startup screen.");
}
inline QString User() { return Root() + QStringLiteral("/user/"); }
inline QString ValidateSetup(const QString& user) {
    QFile ready(QDir(user).filePath(QStringLiteral("setup/ready.json")));
    if(!ready.open(QIODevice::ReadOnly)) return QStringLiteral("Game setup is incomplete. Preserve your user folder and saves, then try a fresh application folder.");
    const auto setup=QJsonDocument::fromJson(ready.readAll()).object();
    if(setup[QStringLiteral("version")].toInt()!=1 || setup[QStringLiteral("rom_sha256")].toString()!=QString::fromLatin1(Hash))
        return QStringLiteral("Game setup is incompatible. Keep your saves and prepare a new application folder.");
    if(setup[QStringLiteral("resource_version")].toInt()!=2 ||
       setup[QStringLiteral("layout_version")].toInt()!=2 ||
       QFileInfo::exists(QDir(user).filePath(QStringLiteral("setup/resource-upgrade-pending"))))
        return QStringLiteral("Game resource upgrade is incomplete. Close Dekui and launch again to retry. Preserve your user folder and its upgrade backups.");
    return {};
}
inline QString Validate() {
    QFile file(Rom());
    if (!file.open(QIODevice::ReadOnly))
        return MissingGameMessage();
    const auto header = file.read(512);
    if (header.size()!=512 || header.mid(0x100,4)!="NCSD") return QStringLiteral("Unsupported game container.");
    const auto word = [&](int at) { return qFromLittleEndian<quint32>(header.constData()+at); };
    const quint64 base=quint64(word(0x120))*512, size=quint64(word(0x124))*512;
    if(base<512 || size!=654643200 || base+size>quint64(file.size()) || !file.seek(base))
        return QStringLiteral("Unsupported or incomplete game dump.");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (quint64 remaining=size; remaining;) {
        const auto bytes=file.read(qint64(qMin<quint64>(remaining,1024*1024)));
        if(bytes.isEmpty()) return QStringLiteral("Could not read the game.");
        hash.addData(bytes); remaining-=bytes.size();
    }
    if(hash.result().toHex()!=Hash) return QStringLiteral("Unsupported game revision/content. The supported decrypted USA Rev 1 dump is required.");
    return ValidateSetup(User());
}
}
