// SPDX-License-Identifier: GPL-2.0-or-later
#include "classic_saving.h"
#include "classic_save_store.h"
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>
using namespace MM3D::ClassicSaving;
void Require(bool ok,const char* name){if(!ok){std::cerr<<name<<": "<<Status()<<'\n';std::exit(1);}}
QByteArray Snapshot(bool owl,char id) {
    QByteArray bytes(Store::SnapshotSize,id);bytes.replace(0,4,QByteArray("MJR\x08",4));
    bytes[0x351e]=owl?1:0;bytes[0x1a88]=0;bytes[0x1a89]=0;
    unsigned crc=0x1017;
    for(unsigned char c:bytes){crc^=c;for(int b=0;b<8;++b)crc=(crc>>1)^((crc&1)?0xa001:0);}
    bytes[0x1a88]=crc&255;bytes[0x1a89]=(crc>>8)&255;return bytes;
}
void Write(const QString& path,const QByteArray& bytes){QFile f(path);Require(f.open(QIODevice::WriteOnly),"write fixture");Require(f.write(bytes)==bytes.size(),"fixture bytes");}
QByteArray Read(const QString& path){QFile f(path);Require(f.open(QIODevice::ReadOnly),"read fixture");return f.readAll();}
int main() {
    for(bool classic:{false,true})for(bool pending:{false,true})for(bool dialogue:{false,true}) {
        enabled=classic;resume_notice=pending;resume_text_active=dialogue;
        std::uint32_t buttons=0xffffffff;std::int16_t x=154,y=-154;
        FilterArrivalPad(buttons,x,y);
        Require(buttons==(classic&&pending?0u:0xffffffffu),"arrival buttons scoped to verified pending notice");
        Require(x==(classic&&pending?0:154)&&y==(classic&&pending?0:-154),"arrival stick cannot move or turn Link");
    }
    resume_notice=false;resume_text_active=false;
    enabled=true;
    QTemporaryDir temp;Require(temp.isValid(),"temp");
    const QString dir=temp.path()+"/title/00040000/00125500/data/00000001";
    Require(QDir().mkpath(dir),"directory");const QString file=dir+"/save0.bin";
    const auto cycle=Snapshot(false,'C'),owl=Snapshot(true,'O');
    Write(file,cycle);RegisterPath(file.toStdString());
    Require(!BeginWrite(0,Kind::Owl),"must first cycle save");
    Require(BeginWrite(0,Kind::Cycle)&&FinishWrite(0,Kind::Cycle),"cycle checkpoint");
    Require(BeginWrite(0,Kind::Owl),"armed owl");Write(file,owl);
    Require(FinishWrite(0,Kind::Owl),"suspend committed");
    Require(PendingMask()==1,"pending owl marker");
    enabled=false;Require(PendingMask()==0,"disabled classic hides marker");enabled=true;
    auto span=[](const QByteArray& b){return std::span(reinterpret_cast<const std::uint8_t*>(b.data()),size_t(b.size()));};
    Require(Resume(0,span(owl)),"consume actual resume");Require(Read(file)==cycle,"native cycle restored");
    Require(PendingMask()==0,"consumed owl marker cleared");
    Require(resume_notice,"verified owl resume arms notice");
    Require(Resume(0,span(owl))&&Read(file)==cycle,"second resume cannot recreate suspend");
    Require(!resume_notice,"repeated load cannot replay notice");
    Require(BeginWrite(0,Kind::Owl),"next suspend");Write(file,owl);Require(FinishWrite(0,Kind::Owl),"next owl");
    const auto replacement=Snapshot(false,'N');Write(file,replacement);
    RegisterPath(file.toStdString());Require(PendingMask()==0,"replacement does not inherit stale marker");
    Require(Resume(0,span(replacement))&&Read(file)==replacement,"replaced slot not resurrected");
    Require(!BeginWrite(0,Kind::Owl),"copied slot requires new cycle checkpoint");
    Require(FinishWrite(0,Kind::Cycle),"replacement cycle activates safely");
    auto corrupt=replacement;corrupt[99]^=1;Write(file,corrupt);
    Require(!FinishWrite(0,Kind::Cycle),"reject invalid native CRC");
    Store store(dir+"/dekui-classic");Store::State state;QString error;
    Require(store.Read(0,state,error)&&state.permanent==replacement,"corrupt save does not replace permanent");
    Write(file,replacement);Require(BeginWrite(0,Kind::Owl),"failure fixture armed");
    Write(file,owl);Require(FinishWrite(0,Kind::Owl),"failure fixture suspend");
    // A failed read/copy may leave old CommonData. A mismatch cannot consume.
    Require(Resume(0,span(cycle))&&Read(file)==owl,"load byte mismatch preserves owl");
    Require(!resume_notice,"mismatch does not arm notice");
    std::cout<<"Classic native-file coordinator: cycle, owl, consume, repeated resume, copy/erase identity and CRC checks passed.\n";
}
