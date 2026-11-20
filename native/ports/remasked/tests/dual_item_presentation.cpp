// SPDX-License-Identifier: GPL-2.0-or-later
#include "item_preview.h"
#include "dialogue_scale.h"
#include <cassert>
#include <iostream>
#include <map>

struct Memory {
    using U32 = uint32_t;
    std::map<U32, U32> words;
    unsigned reads = 0, writes = 0;
    std::optional<U32> Read(U32 at) {
        ++reads;
        auto found = words.find(at);
        return found == words.end() ? std::nullopt : std::optional<U32>(found->second);
    }
    void Write(U32 at, U32 value) { ++writes; words[at] = value; }
};

int main() {
    namespace Preview = Mm3dDekui::ItemPreview;
    Memory memory;
    constexpr uint32_t matrix = 0x10010000, owner = 0x10020000;
    for (unsigned i = 0; i < 12; ++i)
        memory.words[matrix + i * 4] = std::bit_cast<uint32_t>(i % 5 == 0 ? 1.f : 0.f);
    const auto original = memory.words;
    Preview::Hook preview;
    preview.page = 0x10030000;
    preview.owner = owner;
    Preview::model_x = 4.11f;
    Preview::model_y = .44f;
    Preview::model_scale = .87f;
    preview.ModelFrame(owner);
    preview.ModelBegin(memory, owner, matrix);
    assert(memory.words != original); // Single Screen keeps the configured placement.
    preview.ModelEnd(memory);
    assert(memory.words == original);
    const auto writes = memory.writes;
    preview.ModelFrame(owner, true);
    preview.ModelBegin(memory, owner, matrix);
    preview.ModelEnd(memory);
    assert(memory.words == original && memory.writes == writes);
    assert(preview.owner == owner); // Description ownership survives the mode switch.
    assert(Preview::model_x == 4.11f && Preview::model_scale == .87f);
    preview.ModelFrame(owner);
    preview.ModelBegin(memory, owner, matrix);
    assert(memory.words != original); // Switching back restores the same preference.
    preview.ModelEnd(memory);

    Mm3dDekui::DialogueScale::Hook dialogue;
    const auto before = memory.words;
    const auto reads = memory.reads;
    dialogue.Begin(memory, 0x10040000, true, false, nullptr, true);
    dialogue.Begin(memory, 0x10050000, true, false, nullptr, true);
    dialogue.End(memory, 0x10050000);
    dialogue.End(memory, 0x10040000);
    assert(dialogue.frames.empty() && memory.words == before && memory.reads == reads);
    std::cout << "PASS dual item model/description native placement, balanced scopes and single-screen preference retention\n";
}
