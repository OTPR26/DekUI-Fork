// SPDX-License-Identifier: GPL-2.0-or-later
#define main native_inventory_checks_main
#include "native_inventory_check.cpp"
#undef main

int main() {
    namespace F=MM3D::NativeFileSelect;
    Memory m;F::Hook hook;F::Options options;
    options.header={10,-70,.5f,2.f};F::Apply(options);
    const auto manager=m.Alloc(32),bottom=m.Alloc(0x848);
    const auto panel=m.Layout("FileSelect"),header=m.Layout("SubMenuTitle");
    const auto plate=m.Widget("menu_title_base",0,0),shadow=m.Widget("ber_shadow",0,32);
    const auto title=m.Widget("title_text_hint_menu",30,15);
    const auto list=m.Alloc(12);
    m.words[header+0x154]=list;m.words[header+0x158]=3;
    m.words[list]=plate;m.words[list+4]=shadow;m.words[list+8]=title;
    m.words[manager+28]=bottom;m.Queue(bottom,{panel});
    const auto native=m.words;
    hook.Pass(m,manager,0);assert(hook.header_active);
    hook.Begin(m,header);
    assert(std::bit_cast<float>(m.words[plate+0xec])==110.f);
    assert(std::bit_cast<float>(m.words[plate+0xfc])==-70.f);
    assert(std::bit_cast<float>(m.words[plate+0xe0])==.5f);
    assert(std::bit_cast<float>(m.words[plate+0xf4])==2.f);
    assert(std::bit_cast<float>(m.words[shadow+0xfc])==-6.f);
    assert(std::bit_cast<float>(m.words[title+0xec])==125.f);
    assert(std::bit_cast<float>(m.words[title+0xfc])==-40.f);
    assert(std::bit_cast<float>(m.words[title+0xe0])==.5f);
    assert(std::bit_cast<float>(m.words[title+0xf4])==2.f);
    hook.End(m);assert(m.words==native);
    // Shared title on Options or gameplay must never inherit file-select edits.
    auto menu_options=m.Layout("MenuOption");m.Queue(bottom,{panel,menu_options});
    hook.Pass(m,manager,0);assert(!hook.header_active);
    auto before=m.words;hook.Begin(m,header);hook.End(m);assert(m.words==before);
    m.Queue(bottom,{});hook.Pass(m,manager,0);assert(!hook.header_active);
    before=m.words;hook.Begin(m,header);hook.End(m);assert(m.words==before);
    // Missing shadow fails atomically rather than leaving a half-edited header.
    m.Queue(bottom,{panel});m.words[header+0x158]=1;
    hook.Pass(m,manager,0);before=m.words;
    hook.Begin(m,header);hook.End(m);assert(m.words==before);
    std::cout<<"File-select header transforms, restoration, text preservation and screen scope passed.\n";
}
