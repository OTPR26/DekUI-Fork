#define main inventory_fixture_main
#include "native_inventory_check.cpp"
#undef main
#include "mm3d/classic_file_marker.h"
int main() {
    namespace C=MM3D::ClassicSaving;
    Memory m;MM3D::NativeFileSelect::Hook files;C::FileMarker marker;C::enabled=true;
    files.active=true;files.pass=3;files.panel=m.Layout("FileSelect");
    const auto card=m.Layout("FilePlate"),plate=m.Widget("plate_x_n"),name=m.Widget("name",40,20),icon=m.Widget("icon");
    m.words[plate+0x20]=card;m.Children(files.panel+8,{plate});
    const auto widgets=m.Alloc(4);m.words[widgets]=plate;m.words[files.panel+0x154]=widgets;m.words[files.panel+0x158]=1;
    const auto geo=m.Alloc(0x30c),ig=m.Alloc(0x7c),glyphs=m.Alloc(120);
    m.words[name+0x28]=geo;m.words[geo+4]=3;m.words[geo+0x2fc]=glyphs;
    m.words[geo+0x300]=m.words[geo+0x308]=2;m.words[geo+0x304]=256;
    m.words[icon+0x28]=ig;m.words[ig+4]=6;
    for(unsigned i=0;i<2;++i){m.words[glyphs+60*i+20]=std::bit_cast<U32>(float(i*8));m.words[glyphs+60*i+32]=std::bit_cast<U32>(8.f);}
    for(U32 n:{name,card+8,files.panel+8})m.words[n+0x134]=std::bit_cast<U32>(1.f);
    m.Children(card+8,{name,icon});
    const auto groups=m.Alloc(8),resources=m.Alloc(64),texture=m.Alloc(0x60);
    m.words[MM3D::NotebookPrompt::RegistryLiteral]=MM3D::NotebookPrompt::Registry;
    m.words[MM3D::NotebookPrompt::Registry+0x28]=groups;m.words[MM3D::NotebookPrompt::Registry+0x2c]=resources;
    m.words[groups]=16;m.words[resources+60]=texture;m.words[texture+4]=15u<<16;
    const auto stack=(m.Alloc(0x3010)+0x3000)&~7u;const auto before=m.words;
    auto draw=[&](unsigned mask,std::uint64_t t){return marker.Root(m,files,files.panel,files.panel+8,stack,mask,t);};
    assert(draw(0,0)==files.panel+8);marker.scratch.End(m);
    assert(draw(2,0)==files.panel+8);marker.scratch.End(m);
    const auto root=draw(1,0);assert(root!=files.panel+8);
    const auto flag=m.words[root+0x144],fg=m.words[flag+0x28];
    const auto glow=m.words[root+0x140],gg=m.words[glow+0x28];
    assert(m.words[root+0x18]==3);
    assert(m.words[gg+0x28]==std::bit_cast<U32>(344.f/512));
    assert(m.words[gg+0x20]==std::bit_cast<U32>(18.f));
    assert(m.words[gg+0x14]==std::bit_cast<U32>(19.f));
    assert(m.words[glow+0x134]==std::bit_cast<U32>(1.f));
    assert(m.words[fg+0x10]==texture+12);
    assert(m.words[fg+0x14]==std::bit_cast<U32>(20.f));
    assert(m.words[fg+0x18]==std::bit_cast<U32>(-3.f));
    assert(m.words[flag+0xec]==m.words[name+0xec]);
    assert(m.words[fg+0x28]==std::bit_cast<U32>(368.f/512));
    for(unsigned c=0;c<3;++c)assert(m.words[flag+0x128+c*4]==std::bit_cast<U32>(1.f));
    marker.scratch.End(m);assert(m.words==before);
    draw(1,600000);
    assert(m.words[flag+0x130]==std::bit_cast<U32>(1.f));
    assert(m.words[flag+0x134]==std::bit_cast<U32>(1.f));
    assert(std::bit_cast<float>(m.words[glow+0x134])<.001f);
    marker.scratch.End(m);assert(m.words==before);
    // Detailed card uses its displayed number, independent of stale panel
    // transition state, including a different selected slot and Options return.
    files.summary=card;files.pass=2;files.selected_slot=2;
    const auto players=m.Alloc(4),player=m.Alloc(0x18),anim=m.Alloc(0x18);
    m.words[card+0x14c]=players;m.words[card+0x150]=1;m.words[players]=player;
    m.words[player+0xc]=anim;m.words[anim+8]=m.String("switch_file_num_ani");
    for(unsigned slot=0;slot<3;++slot) {
        m.words[player+0x10]=std::bit_cast<U32>(float(slot));
        const auto before_detail=m.words;
        assert(marker.DetailSlot(m,card)==slot);
        auto detail=[&](unsigned mask){return marker.Root(m,files,card,card+8,stack,mask,0);};
        assert(detail(1u<<slot)!=card+8);marker.scratch.End(m);assert(m.words==before_detail);
        assert(detail(1u<<((slot+1)%3))==card+8);marker.scratch.End(m);assert(m.words==before_detail);
    }
    m.words[player+0x10]=std::bit_cast<U32>(3.f);assert(marker.DetailSlot(m,card)==3);
    files.pass=3;
    C::enabled=false;assert(draw(1,0)==files.panel+8);marker.scratch.End(m);
    std::cout<<"Classic flag slot gating, atlas coordinates, name anchoring, pulse and restoration passed.\n";
}
