#include "triaevum_title_whole_aot_abi.h"
#include "oot3d_native_a32_memory.h"
#include <windows.h>
#include <iostream>
#include <memory>
#include <vector>

using namespace Oot3dNativeGame;
namespace a32 = oot3d::recomp::a32;

struct Observer {
    const Oot3dWholeAotProgramV2* Program;
    bool Exit;
    bool SawCalleeState = false;
};

void Observe(uint32_t pc, a32::GuestState& guest, a32::MemoryBus&, void* user) {
    auto& observer = *static_cast<Observer*>(user);
    observer.SawCalleeState = pc == 0x1084 && guest.r[0] == 7;
    if (observer.Exit) observer.Program->ObservableExit(pc, nullptr);
    guest.r[0] = 20;
}

void RecordHook(uint32_t pc, a32::GuestState&, a32::MemoryBus&, void* user) {
    static_cast<std::vector<uint32_t>*>(user)->push_back(pc);
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const auto module = LoadLibraryExW(argv[1], nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) return 3;
    const auto query = reinterpret_cast<const Oot3dWholeAotProgramV2* (*)(uint32_t) noexcept>(
        GetProcAddress(module, "triaevum_title_whole_aot_query"));
    const auto program = query ? query(kOot3dWholeAotPluginAbiV2) : nullptr;
    if (!program || program->StructSize != sizeof(*program) ||
        program->MemoryWriteObservationVersion != NativeA32Memory::WriteObservationAbiVersion()) return 4;
    auto memory = std::make_unique<NativeA32Memory>();
    NativeA32MemoryRegionConfig region;
    region.Name = "synthetic-data";
    region.BaseAddress = 0x3000;
    region.Size = 4096;
    region.Writable = true;
    if (!memory->MapRegion(region)) return 5;
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        if (!memory->Write32(0x3000, 0)) return 6;
        const auto beforeStore = memory->RangeWriteGeneration(0x3000, 4);
        a32::GuestState guest{};
        guest.cpsr = 0x10;
        guest.r[3] = scenario == 3 ? 0 : 0x3000;
        guest.r[14] = 0x2000;
        a32::ExecutionResult result{};
        Oot3dWholeAotStats stats{};
        uint32_t consumed = 0, stored = 0;
        const uint32_t observedPc = 0x1084;
        Observer observer{program, scenario == 2};
        const bool hooked = scenario == 1 || scenario == 2;
        const bool handled = program->Execute(0x1000, guest, *memory, &result, &stats, nullptr,
            64, &consumed, hooked ? Observe : nullptr, &observer,
            hooked ? &observedPc : nullptr, hooked ? 1 : 0, nullptr, false, 0x2000);
        if (!handled || !memory->Read32(0x3000, &stored) || program->ExecutionActive()) return 7;
        if (!beforeStore ||
            ((memory->RangeWriteGeneration(0x3000, 4) != beforeStore) != (scenario < 2))) {
            std::cerr << "compiled stores and native page certificates disagree\n";
            return 8;
        }
        bool matches = false;
        switch (scenario) {
        case 0:
            matches = guest.r[0] == 16 && guest.r[1] == 42 && guest.r[2] == 17 &&
                stored == 17 && (guest.cpsr & 0x40000000U) && result.pc == 0x2000;
            break;
        case 1:
            matches = observer.SawCalleeState && guest.r[0] == 29 && guest.r[1] == 99 &&
                guest.r[2] == 30 && stored == 30 && !(guest.cpsr & 0x40000000U) && result.pc == 0x2000;
            break;
        case 2:
            matches = observer.SawCalleeState && guest.r[0] == 7 && guest.r[2] == 0 &&
                stored == 0 && result.pc == 0x1084;
            break;
        case 3:
            matches = guest.r[0] == 7 && guest.r[2] == 17 && stored == 0 &&
                result.kind == a32::ExitKind::MemoryFault && result.pc == 0x1088 && result.detail == 0;
            break;
        }
        if (!matches) {
            std::cerr << "scenario=" << scenario << " r0=" << guest.r[0] << " r1=" << guest.r[1]
                      << " r2=" << guest.r[2] << " stored=" << stored << " cpsr=" << guest.cpsr
                      << " pc=" << result.pc << " kind=" << unsigned(result.kind) << '\n';
            return 10 + scenario;
        }
    }
    for (uint32_t input : {1U, 2U}) {
        a32::GuestState guest{};
        guest.cpsr = 0x10;
        guest.r[0] = input;
        guest.r[14] = 0x2000;
        a32::ExecutionResult result{};
        Oot3dWholeAotStats stats{};
        uint32_t consumed = 0;
        const bool handled = program->Execute(0x10C0, guest, *memory, &result, &stats,
            nullptr, 64, &consumed, nullptr, nullptr, nullptr, 0, nullptr, false, 0x2000);
        if (!handled || result.kind != a32::ExitKind::Branch || result.pc != 0x2000 ||
            guest.r[0] != input || guest.r[1] != (input == 1 ? 42U : 99U) ||
            bool(guest.cpsr & 0x40000000U) != (input == 1)) {
            std::cerr << "conditional return lost committed registers or flags\n";
            return 20;
        }
    }
    // Budget exhaustion wins over notification. Skip-first applies only to
    // the first consumed block, and a filter collision is not an exact hook.
    uint32_t collision = 0x10C4;
    while (Oot3dAotBlockEntryFilter::BitIndex(collision) !=
           Oot3dAotBlockEntryFilter::BitIndex(0x10C0)) {
        collision += 4;
    }
    struct HookCase {
        uint32_t Budget;
        bool SkipFirst;
        bool Filtered;
        std::vector<uint32_t> Expected;
        uint32_t ExitPc;
    };
    const HookCase hookCases[] = {
        {1, true, false, {}, 0x10CC},
        {2, true, false, {0x10CC}, 0x2000},
        {2, false, false, {0x10C0, 0x10CC}, 0x2000},
        {2, false, true, {}, 0x2000},
    };
    for (const auto& scenario : hookCases) {
        a32::GuestState guest{};
        guest.cpsr = 0x10;
        guest.r[0] = 2;
        guest.r[14] = 0x2000;
        a32::ExecutionResult result{};
        Oot3dWholeAotStats stats{};
        uint32_t consumed = 0;
        std::vector<uint32_t> observed;
        Oot3dAotBlockEntryFilter filter;
        if (scenario.Filtered) filter.Insert(collision);
        const bool handled = program->Execute(
            0x10C0, guest, *memory, &result, &stats, nullptr,
            scenario.Budget, &consumed, RecordHook, &observed,
            scenario.Filtered ? &collision : nullptr, scenario.Filtered ? 1 : 0,
            &filter, scenario.SkipFirst, 0x2000);
        if (!handled || consumed != scenario.Budget ||
            observed != scenario.Expected || result.pc != scenario.ExitPc ||
            guest.r[0] != 2 || guest.r[1] != (scenario.Budget == 2 ? 99U : 0U)) {
            std::cerr << "block budget or hook precedence changed: budget="
                      << scenario.Budget << " consumed=" << consumed
                      << " skip=" << scenario.SkipFirst << " filtered=" << scenario.Filtered
                      << " hooks=" << observed.size() << " pc=" << result.pc << '\n';
            return 21;
        }
    }
    std::cout << "{\"nested_return_and_flags\":true,\"observer_reload\":true,"
                 "\"snapshotless_unwind\":true,\"callee_memory_fault\":true,"
                 "\"conditional_return\":true,\"compiled_page_epochs\":true,"
                 "\"block_hook_budget_boundaries\":true}\n";
    FreeLibrary(module);
}
