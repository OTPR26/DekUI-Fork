// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QByteArray>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QSaveFile>
#include <QString>
#include <QUuid>

namespace MM3D::ClassicSaving {
// Storage only: not wired to the guest yet. Callers must validate native snapshot
// checksums and observe successful semantic save/load events. Never infer those
// events from a file timestamp or the in-memory owl flag.
class Store {
public:
    static constexpr int SnapshotSize = 0x13618;
    struct State {
        QByteArray permanent, suspend, receipt;
    };
    explicit Store(QString directory) : directory{std::move(directory)} {}

    bool Read(unsigned slot, State& out, QString& error) const {
        if (!ValidSlot(slot, error)) return false;
        QFile file(Path(slot));
        if (!file.exists()) { out = {}; return true; }
        if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * SnapshotSize + 256) {
            error = "Cannot read classic save checkpoint."; return false;
        }
        const auto bytes = file.readAll();
        if (bytes.size() < 32 || Digest(bytes.mid(32)) != bytes.first(32)) {
            error = "Classic save checkpoint checksum failed."; return false;
        }
        QDataStream stream(bytes.mid(32));
        stream.setVersion(QDataStream::Qt_6_0);
        quint32 magic=0, version=0, stored_slot=0;
        // Fixed bounded payloads avoid deserializing attacker-controlled lengths.
        stream >> magic >> version >> stored_slot;
        quint8 pending=0;
        stream >> pending;
        State state;
        state.permanent.resize(SnapshotSize);
        const bool main_ok = stream.readRawData(state.permanent.data(), SnapshotSize) == SnapshotSize;
        if (pending == 1) {
            state.receipt.resize(16); state.suspend.resize(SnapshotSize);
            if (stream.readRawData(state.receipt.data(), 16) != 16 ||
                stream.readRawData(state.suspend.data(), SnapshotSize) != SnapshotSize) {
                error = "Truncated classic suspend checkpoint."; return false;
            }
        }
        if (magic != 0x444b4353 || version != 1 || stored_slot != slot || pending > 1 ||
            !main_ok || stream.status() != QDataStream::Ok || !stream.atEnd()) {
            error = "Invalid classic save checkpoint."; return false;
        }
        out = std::move(state);
        return true;
    }

    // Only after a verified new-cycle save. Also retires any old suspend token.
    bool CommitCycle(unsigned slot, const QByteArray& snapshot, QString& error) {
        return Update(slot, error, [&](State& state) {
            if (!Snapshot(snapshot, error)) return false;
            state = {snapshot, {}, {}}; return true;
        });
    }
    bool CommitSuspend(unsigned slot, const QByteArray& snapshot, QString& error) {
        return Update(slot, error, [&](State& state) {
            if (!Snapshot(snapshot, error)) return false;
            if (state.permanent.isEmpty()) {
                error = "A successful new-cycle save is required before classic owl saving.";
                return false;
            }
            state.suspend = snapshot;
            state.receipt = QUuid::createUuid().toRfc4122();
            return true;
        });
    }
    // Read is non-consuming: file-select previews, cancelled or failed loads
    // cannot retire a suspend. Commit consumption after verified successful load
    // and before resuming guest execution; failure must keep execution paused.
    bool Consume(unsigned slot, const QByteArray& receipt, QString& error) {
        return Update(slot, error, [&](State& state) {
            if (receipt.size() != 16 || state.receipt != receipt || state.suspend.isEmpty()) {
                error = "Classic suspend receipt is stale or already consumed."; return false;
            }
            state.suspend.clear(); state.receipt.clear(); return true;
        });
    }
private:
    QString directory;
    static QByteArray Digest(const QByteArray& data) {
        return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    }
    QString Path(unsigned slot) const {
        return QDir(directory).filePath(QString("slot-%1.dkcs").arg(slot));
    }
    static bool ValidSlot(unsigned slot, QString& error) {
        if (slot < 3) return true;
        error = "Invalid classic save slot."; return false;
    }
    static bool Snapshot(const QByteArray& data, QString& error) {
        if (data.size() == SnapshotSize) return true;
        error = "Unexpected native save snapshot size."; return false;
    }
    template<class Edit> bool Update(unsigned slot, QString& error, Edit edit) {
        error.clear();
        if (!ValidSlot(slot, error)) return false;
        if (!QDir().mkpath(directory)) { error = "Cannot create classic save directory."; return false; }
        QLockFile lock(Path(slot) + ".lock");
        if (!lock.tryLock(0)) { error = "Classic save slot is locked."; return false; }
        State state;
        if (!Read(slot, state, error) || !edit(state)) return false;
        QByteArray payload;
        QDataStream stream(&payload, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_6_0);
        stream << quint32(0x444b4353) << quint32(1) << quint32(slot)
               << quint8(!state.suspend.isEmpty());
        stream.writeRawData(state.permanent.data(), state.permanent.size());
        if (!state.suspend.isEmpty()) {
            stream.writeRawData(state.receipt.data(), state.receipt.size());
            stream.writeRawData(state.suspend.data(), state.suspend.size());
        }
        if (stream.status() != QDataStream::Ok) { error = "Cannot encode classic save."; return false; }
        QSaveFile file(Path(slot));
        file.setDirectWriteFallback(false);
        const auto bytes = Digest(payload) + payload;
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            error = "Cannot commit classic save checkpoint; previous checkpoint retained.";
            return false;
        }
        return true;
    }
};
} // namespace MM3D::ClassicSaving
