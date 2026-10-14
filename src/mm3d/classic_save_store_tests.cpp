// SPDX-License-Identifier: GPL-2.0-or-later
#include "classic_save_store.h"
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>
using MM3D::ClassicSaving::Store;
static void Require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    QTemporaryDir temp;
    Require(temp.isValid(), "temporary directory");
    Store store(temp.path()); QString error; Store::State state;
    const QByteArray cycle(Store::SnapshotSize, 'C'), owl(Store::SnapshotSize, 'O');
    Require(!store.CommitSuspend(0, owl, error), "must not invent permanent checkpoint");
    for (unsigned slot=0;slot<3;++slot) {
        Require(store.CommitCycle(slot,cycle,error), "commit cycle");
        Require(store.CommitSuspend(slot,owl,error), "commit suspend");
    }
    Require(store.Read(0,state,error), "read suspend");
    const auto first=state.receipt;
    Require(state.permanent==cycle && state.suspend==owl,"preserve both snapshots");
    Store restarted(temp.path());
    Require(restarted.Read(0,state,error) && state.receipt==first,"preview/restart does not consume");
    Require(!store.Consume(0,QByteArray(16,'x'),error),"wrong receipt fails");
    Require(store.Consume(0,first,error),"consume verified resume");
    Require(!store.Consume(0,first,error),"cannot resume twice");
    Require(restarted.Read(0,state,error) && state.suspend.isEmpty() && state.permanent==cycle,
            "permanent survives consumption across restart");
    Require(store.Read(1,state,error) && state.suspend==owl,"slots isolated");
    const auto old_slot_one=state.receipt;
    Require(store.CommitSuspend(1,owl,error),"replace suspend");
    Require(!store.Consume(1,old_slot_one,error),"stale receipt cannot consume newer suspend");
    Require(store.Read(1,state,error),"read new receipt");
    const auto new_slot_one=state.receipt;
    {
        QLockFile lock(temp.filePath("slot-1.dkcs.lock"));
        Require(lock.tryLock(),"consumption lock fixture");
        Require(!store.Consume(1,new_slot_one,error),"failed consumption must fail closed");
    }
    Require(restarted.Read(1,state,error) && state.receipt==new_slot_one && state.suspend==owl,
            "failed consumption preserves eligibility");
    Require(store.CommitCycle(1,cycle,error),"cycle retires suspend");
    Require(store.Read(1,state,error) && state.suspend.isEmpty(),"cycle reset");
    Require(!store.CommitCycle(0,QByteArray(3,'x'),error),"reject truncated native snapshot");
    Require(!store.CommitCycle(3,cycle,error),"slot bounds");
    {
        QLockFile lock(temp.filePath("slot-0.dkcs.lock"));
        Require(lock.tryLock(),"lock fixture");
        Require(!store.CommitSuspend(0,owl,error),"parallel writer rejected");
    }
    Require(store.Read(0,state,error) && state.permanent==cycle && state.suspend.isEmpty(),
            "failed updates preserve data");
    // Corrupt input must not be silently overwritten, even by a new cycle.
    QFile corrupt(temp.filePath("slot-2.dkcs"));
    Require(corrupt.open(QIODevice::ReadWrite),"corruption fixture");
    Require(corrupt.seek(100) && corrupt.write("!")==1,"corrupt byte");corrupt.close();
    Require(!store.Read(2,state,error),"detect corruption");
    Require(!store.CommitCycle(2,cycle,error),"preserve corrupt evidence");
    // A blocked destination must report failure without affecting another store.
    Require(store.Read(0,state,error),"failure fixture read");
    const auto path=temp.filePath("not-a-directory");
    QFile blocker(path);Require(blocker.open(QIODevice::WriteOnly),"file blocks directory");blocker.close();
    Store unavailable(path);
    Require(!unavailable.CommitCycle(0,cycle,error),"unwritable storage fails");
    Require(store.Read(0,state,error) && state.permanent==cycle,"other slot store untouched");
    std::cout << "Classic save storage: checkpoints, one-use receipts, restart, isolation, locking and corruption checks passed.\n";
}
