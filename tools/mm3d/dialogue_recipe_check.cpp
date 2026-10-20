// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/dialogue_recipe.h"
#include <cassert>
#include <unordered_map>
using namespace MM3D::DialogueText;
bool DialogueCacheShared();
struct Memory {
    std::unordered_map<std::uint32_t,std::uint32_t> words;
    auto Read(std::uint32_t at) {
        auto i=words.find(at);
        return i==words.end()?std::optional<std::uint32_t>{}:std::optional{i->second};
    }
};
int main() {
    const std::string native="Synthetic fixture: red",edited="Synthetic fixture: blue";
    constexpr char commands[]="\000\000\023\000\000\000\000\000\000\000\004\000blue";
    Recipe original{std::uint32_t(native.size()),Hash(native),std::uint32_t(native.size()),Hash(native),{}};
    Recipe changed{original.source_size,original.source_hash,std::uint32_t(edited.size()),Hash(edited),
                   {commands,sizeof(commands)-1}};
    assert(!Resolve(changed,edited)); // No guessed original or saved bundled fallback.
    assert(!Resolve(changed,"invalid"));
    assert(Resolve(changed,native)==edited);
    assert(DialogueCacheShared());
    assert(Resolve(original,edited)==native); // Live toggle restores user-supplied bytes.
    assert(Resolve(changed,edited)==edited); // Idempotent on the modified message.
    auto bad=changed;bad.hash^=1;assert(!Resolve(bad,native));
    bad=changed;bad.commands=bad.commands.substr(0,5);assert(!Resolve(bad,native));
    constexpr char outside[]="\377\377\001\000\000\000";
    bad=changed;bad.commands={outside,sizeof(outside)-1};assert(!Resolve(bad,native));
    bad=changed;bad.length=1;assert(!Resolve(bad,native));
    bad=original;bad.length--;assert(!Resolve(bad,native));
    bad=original;bad.source_size=4097;assert(!Resolve(bad,native));
    Memory m{{{0x100,0x44332211},{0x104,0x88776655}}};
    assert(Read(m,0x102,4)==std::string("\x33\x44\x55\x66",4));
    assert(!Read(m,0x106,4)); // An unreadable final word cannot be used as an original.
    assert(!Read(m,0xffffffff,8));
}
