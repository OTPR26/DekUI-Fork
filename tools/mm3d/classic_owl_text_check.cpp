// SPDX-License-Identifier: GPL-2.0-or-later
#define main ExistingTextChecks
#include "single_screen_text_check.cpp"
#undef main
int main(int argc,char** argv) {
    assert(argc==2);
    const auto original=Load(argv[1]);
    Memory m(original);
    Replacement replacement=ClassicOwlReplacements[0];
    for(const auto& d:MM3D::ClassicLocalizedText::Dialogues)if(m.Read(Memory::Base+12)==d.language)
        replacement={0xc01,U32(d.original.size()),d.original_hash,d.classic,d.original,d.classic_hash};
    auto entry=m.Entry(0xc01);assert(entry);
    auto offset=*m.Read(entry+12),size=*m.Read(entry+16);
    assert(size==replacement.size);
    MM3D::ClassicSaving::enabled=false;
    Apply(m,Memory::Self,0xc01);assert(m.data==original && !m.writes);
    MM3D::ClassicSaving::enabled=true;
    enabled=false; // independent of the single-screen text checkbox
    Apply(m,Memory::Self,0xc01);assert(m.writes);
    for(unsigned i=0;i<original.size();++i) {
        auto expected=original[i];
        if(i==entry+8-Memory::Base) expected=0; // clear only native instant flag
        if(i>=offset && i<offset+size)
            expected=i-offset<replacement.text.size()?replacement.text[i-offset]:0;
        assert(m.data[i]==expected);
    }
    auto edited=m.data;m.writes=0;Apply(m,Memory::Self,0xc01);
    assert(m.data==edited && !m.writes);
    MM3D::ClassicSaving::enabled=false;
    Apply(m,Memory::Self,0xc01);assert(m.data==original);
    MM3D::ClassicSaving::enabled=true;
    m.data[offset]^=1;auto changed=m.data;m.writes=0;
    Apply(m,Memory::Self,0xc01);assert(m.data==changed && !m.writes);
    Memory custom_flags(original);
    custom_flags.data[entry+8-Memory::Base]^=0x10;
    auto flags_before=custom_flags.data;
    Apply(custom_flags,Memory::Self,0xc01);
    assert(custom_flags.data==flags_before && !custom_flags.writes);
    std::cout<<"Classic owl text: exact payload, isolation, live toggle, idempotence and modified-text guard passed.\n";
}
