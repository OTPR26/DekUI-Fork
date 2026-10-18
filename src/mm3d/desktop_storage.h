// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

namespace MM3D::DesktopStorage {
inline QByteArray Digest(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f)) return {};
    return hash.result().toHex();
}
inline bool CopyVerified(const QString& source, const QString& target, const QString& relative,
                         QJsonArray& manifest) {
    const QFileInfo info(source);
    if (info.isSymLink()) {
        // Keep external ROM/texture references as references; never traverse them.
        const auto link = info.symLinkTarget();
        if (!QFile::link(link, target) || QFileInfo(target).symLinkTarget() != link) return false;
        manifest.append(QJsonObject{{QStringLiteral("path"),relative},{QStringLiteral("symlink"),link}});
        return true;
    }
    if (info.isDir()) {
        if (!QDir().mkpath(target)) return false;
        for (const auto& child : QDir(source).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
            if (!CopyVerified(child.absoluteFilePath(), target + QLatin1Char('/') + child.fileName(),
                              relative + QLatin1Char('/') + child.fileName(), manifest)) return false;
        }
        return true;
    }
    if (!info.isFile() || !QFile::copy(source,target)) return false;
    const auto digest = Digest(source);
    if (digest.isEmpty() || Digest(target) != digest || QFileInfo(target).size() != info.size()) return false;
    manifest.append(QJsonObject{{QStringLiteral("path"),relative},{QStringLiteral("sha256"),QString::fromLatin1(digest)},
                                {QStringLiteral("bytes"),double(info.size())}});
    return true;
}
// Atomic publication, with the portable source retained as a recovery copy.
// Never merge two sets of saves or replace an existing destination.
inline QString Migrate(const QString& source, const QString& destination) {
    const QString marker = destination + QStringLiteral("/.storage-v1.json");
    if (QFileInfo::exists(marker)) {
        QFile f(marker);
        if (f.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("version")).toInt() == 1)
            return {};
        return QStringLiteral("The Application Support storage marker is invalid. Your existing files have been preserved.");
    }
    if (QFileInfo::exists(destination))
        return QStringLiteral("Application Support already contains unrecognized data. No files were replaced; preserve both folders before resolving this conflict.");
    const QFileInfo dest(destination);
    if (!QDir().mkpath(dest.absolutePath())) return QStringLiteral("Cannot create Application Support folder.");
    const QString stage = destination + QStringLiteral(".migration-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(stage)) return QStringLiteral("Cannot stage Application Support migration.");
    QJsonArray manifest;
    auto fail = [&] { QDir(stage).removeRecursively();return QStringLiteral("Could not verify the Application Support migration. Original saves and settings were left untouched."); };
    for (const auto& name : {QStringLiteral("user"), QStringLiteral("desktop-settings.json"), QStringLiteral("desktop-native.ini")}) {
        const QFileInfo entry(source + QLatin1Char('/') + name);
        if (name==QStringLiteral("user") && entry.exists() && (!entry.isDir() || entry.isSymLink())) return fail();
        if ((entry.exists() || entry.isSymLink()) && !CopyVerified(entry.absoluteFilePath(),stage + QLatin1Char('/') + name,name,manifest)) return fail();
    }
    // Rebase only app-owned paths in active settings. External ROM/texture paths
    // remain unchanged. Backups and save/model payloads remain byte-identical.
    QJsonArray rebased;
    const auto oldUser = QDir::cleanPath(source + QStringLiteral("/user"));
    const auto newUser = QDir::cleanPath(destination + QStringLiteral("/user"));
    const auto rebase = [&](const QString& relative) {
        QFile f(stage + QLatin1Char('/') + relative);
        if (!f.exists()) return true;
        if (!f.open(QIODevice::ReadOnly)) return false;
        const auto before=f.readAll();f.close();auto after=before;
        after.replace((oldUser + QLatin1Char('/')).toUtf8(),(newUser + QLatin1Char('/')).toUtf8());
        if (after==before) return true;
        QSaveFile out(stage + QLatin1Char('/') + relative);
        if (!out.open(QIODevice::WriteOnly) || out.write(after)!=after.size() || !out.commit()) return false;
        rebased.append(relative);return true;
    };
    for (const auto& relative : {QStringLiteral("desktop-settings.json"),QStringLiteral("desktop-native.ini"),
            QStringLiteral("user/config/qt-config.ini"),QStringLiteral("user/config/mm3d-local/texture-source.json")})
        if (!rebase(relative)) return fail();
    QSaveFile out(stage + QStringLiteral("/.storage-v1.json"));
    const auto record=QJsonDocument(QJsonObject{{QStringLiteral("version"),1},{QStringLiteral("source"),source},
                         {QStringLiteral("verified_files"),manifest},{QStringLiteral("rebased_settings"),rebased}}).toJson();
    if (!out.open(QIODevice::WriteOnly) || out.write(record)!=record.size() || !out.commit()) return fail();
    if (!QDir().rename(stage,destination)) return fail();
    return {};
}
} // namespace MM3D::DesktopStorage
