// SPDX-License-Identifier: GPL-2.0-or-later
#include "citra_qt/dekui_icloud.h"

#import <Foundation/Foundation.h>

#include <array>
#include <optional>
#include <utility>
#include <vector>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QUuid>

namespace MM3D::DesktopCloud {
namespace {
NSString* const Container = @"iCloud.io.github.otpr26.remasked.azaharprototype.ios";

NSURL* Documents() {
    auto* files = NSFileManager.defaultManager;
    // Existing macOS distributions can use the iPhone app's public iCloud
    // Drive mirror without an Apple development provisioning profile.
    auto* mirror = [NSURL fileURLWithPath:NSHomeDirectory() isDirectory:YES];
    mirror = [mirror URLByAppendingPathComponent:
        @"Library/Mobile Documents/iCloud~io~github~otpr26~remasked~azaharprototype~ios/Documents"
                                      isDirectory:YES];
    if ([files fileExistsAtPath:[mirror URLByAppendingPathComponent:@"ReMasked Data.txt"].path])
        return mirror;
    auto* container = [files URLForUbiquityContainerIdentifier:Container];
    return [container URLByAppendingPathComponent:@"Documents" isDirectory:YES];
}

bool RegularDirectoryPath(const QString& path) {
    for (QString current = QDir(path).absolutePath(); current != QStringLiteral("/");) {
        const QFileInfo info(current);
        if (info.isSymLink() || (info.exists() && !info.isDir()))
            return false;
        current = info.absolutePath();
    }
    return true;
}

struct FileData {
    std::optional<QByteArray> bytes;
    bool valid;
};

FileData ReadFile(const QString& path, qint64 size) {
    const QFileInfo info(path);
    if (info.isSymLink())
        return {{}, false};
    if (!info.exists())
        return {{}, true};
    QFile file(path);
    if (!info.isFile() || info.size() != size || !file.open(QIODevice::ReadOnly))
        return {{}, false};
    auto bytes = file.read(size + 1);
    const bool valid = bytes.size() == size && file.error() == QFileDevice::NoError;
    return {std::move(bytes), valid};
}

QString Hash(const QByteArray& data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool WriteFile(const QString& path, const QByteArray& data) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath()) || QFileInfo(path).isSymLink())
        return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}

bool AwaitDownload(const QString& path) {
    const QFileInfo file(path);
    // A placeholder may be visible as a dot file before Finder materializes
    // the requested revision. Give the system a chance to download it.
    auto* url = [NSURL fileURLWithPath:path.toNSString()];
    if (!file.exists()) {
        const QString placeholder = file.dir().filePath(
            QStringLiteral(".") + file.fileName() + QStringLiteral(".icloud"));
        if (!QFileInfo::exists(placeholder))
            return false;
    }
    NSNumber* ubiquitous = nil;
    NSString* status = nil;
    if ([url getResourceValue:&ubiquitous forKey:NSURLIsUbiquitousItemKey error:nil] &&
        ubiquitous.boolValue &&
        [url getResourceValue:&status forKey:NSURLUbiquitousItemDownloadingStatusKey error:nil] &&
        [status isEqualToString:NSURLUbiquitousItemDownloadingStatusCurrent])
        return false;
    if (ubiquitous.boolValue || !file.exists()) {
        [NSFileManager.defaultManager startDownloadingUbiquitousItemAtURL:url error:nil];
        // An item that has already downloaded is readable even if its provider
        // does not report a status value.
        return !file.exists() || status != nil;
    }
    return false;
}

} // namespace

Result Reconcile(const QString& user_root, const QString& documents, Resolution resolution) {
    const auto local_root = QFileInfo(user_root).canonicalFilePath();
    const auto cloud_root = QFileInfo(documents).canonicalFilePath();
    const QDir local(local_root + QStringLiteral(
        "/sdmc/Nintendo 3DS/00000000000000000000000000000000/"
        "00000000000000000000000000000000/title/00040000/00125500/data"));
    const QDir cloud(cloud_root + QStringLiteral("/saves"));
    if (local_root.isEmpty() || cloud_root.isEmpty() ||
        !QFileInfo(local_root).isDir() || !QFileInfo(cloud_root).isDir() ||
        !RegularDirectoryPath(local.filePath(QStringLiteral("00000001"))) ||
        !RegularDirectoryPath(local.filePath(QStringLiteral("00000001/.classic-saving"))) ||
        !RegularDirectoryPath(cloud.absolutePath()))
        return {QStringLiteral("The save folders contain a symbolic link or an invalid directory.")};

    const QString state_path = local_root + QStringLiteral("/.icloud-saves-v1.json");
    if (QFileInfo(state_path).isSymLink())
        return {QStringLiteral("The local iCloud sync record is invalid.")};
    QLockFile lock(state_path + QStringLiteral(".lock"));
    if (!lock.tryLock())
        return {QStringLiteral("Another ReMasked instance is syncing saves.")};
    QJsonObject baseline;
    QFile state(state_path);
    if (state.exists()) {
        if (!state.open(QIODevice::ReadOnly) || state.size() > 4096)
            return {QStringLiteral("The local iCloud sync record could not be read.")};
        const auto json = QJsonDocument::fromJson(state.readAll());
        if (!json.isObject())
            return {QStringLiteral("The local iCloud sync record is invalid.")};
        baseline = json.object();
        state.close();
    }

    struct Change {
        QString target;
        QByteArray replacement;
        std::optional<QByteArray> original;
        bool preserve_backup;
    };
    std::vector<Change> changes;
    bool protected_checkpoint = false;
    for (const auto& name : {QStringLiteral("save0.bin"), QStringLiteral("save1.bin"),
                             QStringLiteral("save2.bin"), QStringLiteral("00000001.metadata")}) {
        const bool metadata = name.endsWith(QStringLiteral(".metadata"));
        const qint64 size = metadata ? 16 : 79384;
        const auto local_path = local.filePath(metadata ? name : QStringLiteral("00000001/") + name);
        const auto cloud_path = cloud.filePath(name);
        if (AwaitDownload(cloud_path))
            return {QStringLiteral("iCloud Drive is downloading the saves. Try syncing again shortly.")};
        const auto left = ReadFile(local_path, size);
        const auto right = ReadFile(cloud_path, size);
        if (!left.valid || !right.valid)
            return {QStringLiteral("A save file is invalid or unavailable. Both copies were kept.")};
        if (!left.bytes && !right.bytes)
            continue;
        if (left.bytes && right.bytes && *left.bytes == *right.bytes) {
            baseline.insert(name, Hash(*left.bytes));
            continue;
        }
        const auto previous = baseline.value(name).toString();
        bool to_local = !left.bytes;
        if (left.bytes && right.bytes) {
            // Use the same Classic Saving checkpoint safeguard as iOS.
            if (!metadata && Hash(*right.bytes) != previous) {
                const auto receipt = ReadFile(local.filePath(
                    QStringLiteral("00000001/.classic-saving/") + name + QStringLiteral(".cycle")), size);
                if (!receipt.valid)
                    return {QStringLiteral("A Classic Saving checkpoint receipt could not be read.")};
                if (receipt.bytes && *receipt.bytes == *left.bytes) {
                    protected_checkpoint = true;
                    continue;
                }
            }
            if (!previous.isEmpty() && Hash(*left.bytes) == previous)
                to_local = true;
            else if (!previous.isEmpty() && Hash(*right.bytes) == previous)
                to_local = false;
            else if (resolution == Resolution::Ask)
                return {QStringLiteral("Both copies of a save differ. Choose which version to keep for conflicts."), false, true};
            else
                to_local = resolution == Resolution::Drive;
        }
        const auto& replacement = to_local ? *right.bytes : *left.bytes;
        baseline.insert(name, Hash(replacement));
        changes.push_back({to_local ? local_path : cloud_path, replacement,
                           to_local ? left.bytes : right.bytes,
                           to_local || resolution != Resolution::Ask});
    }

    // Preflight all slots before committing any replacement. The directory is
    // coordinated with iCloud Drive and each write is atomic.
    const QDir backups(local_root + QStringLiteral("/Save Backups/iCloud-") +
                       QUuid::createUuid().toString(QUuid::WithoutBraces));
    for (const auto& change : changes) {
        if (change.preserve_backup && change.original &&
            (!RegularDirectoryPath(backups.absolutePath()) ||
             !WriteFile(backups.filePath(QFileInfo(change.target).fileName()), *change.original))) {
            return {QStringLiteral("A previous save could not be backed up. Both copies were kept.")};
        }
    }
    std::size_t written = 0;
    for (const auto& change : changes) {
        const auto current = ReadFile(change.target, change.replacement.size());
        if (!current.valid || current.bytes != change.original ||
            !WriteFile(change.target, change.replacement))
            break;
        ++written;
    }
    if (written != changes.size() || !WriteFile(state_path, QJsonDocument(baseline).toJson())) {
        bool restored = true;
        while (written) {
            const auto& change = changes[--written];
            const bool rollback = change.original ? WriteFile(change.target, *change.original)
                                                  : QFile::remove(change.target);
            restored = rollback && restored;
        }
        return {restored ? QStringLiteral("Sync could not finish. Previous saves were restored.")
                         : QStringLiteral("Sync could not finish and a file could not be restored.")};
    }
    if (protected_checkpoint)
        return {QStringLiteral("Kept the local permanent checkpoint. A different iCloud copy remains."), true};
    return {QStringLiteral("Save slots are up to date with ReMasked on iCloud Drive."), true};
}

QString DriveFolder() {
    @autoreleasepool {
        return QString::fromNSString(Documents().path);
    }
}

Result Sync(const QString& user_root, Resolution resolution) {
    @autoreleasepool {
        auto* folder = Documents();
        if (!folder || !QFileInfo(user_root).isDir())
            return {QStringLiteral("The iPhone’s ReMasked iCloud Drive folder or local game setup is unavailable.")};
        NSError* error = nil;
        __block Result result;
        auto* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
        [coordinator coordinateWritingItemAtURL:folder options:0 error:&error
                                    byAccessor:^(NSURL* coordinated) {
            result = Reconcile(user_root, QString::fromNSString(coordinated.path), resolution);
        }];
        if (error)
            return {QStringLiteral("Could not access the ReMasked folder in iCloud Drive.")};
        return result;
    }
}

} // namespace MM3D::DesktopCloud
