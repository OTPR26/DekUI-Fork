// Developer-only proxy DLL. Never package: it deliberately pauses execution to
// replay one real invocation on private memory copies, not to measure game FPS.
#include "triaevum_title_whole_aot_abi.h"
#include "oot3d_native_a32_memory.h"
#include "dynarmic_replay_bridge.h"
#include "aot_replay_memory.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <bit>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <sstream>

using namespace Oot3dNativeGame;
namespace a32 = oot3d::recomp::a32;
namespace {
const Oot3dWholeAotProgramV2* baseline;
const Oot3dWholeAotProgramV2* candidate;
const DynarmicReplayApi* dynarmic;
uint32_t target;
unsigned occurrences;
unsigned captureOccurrence = 64;
bool completed;
struct Target { uint32_t Pc; unsigned Seen=0; bool Done=false; };
std::vector<Target> batchTargets;
void Lifecycle(const char* phase) {
    if(const char* path=std::getenv("TRIAEVUM_INVOCATION_OUTPUT"))
        std::ofstream(std::string(path)+".lifecycle",std::ios::app) << phase << '\n';
}
using InputSelector = bool (*)(const a32::GuestState*, const NativeA32Memory*) noexcept;
InputSelector inputSelector;
struct Route {
    a32::BlockEntryCallback Callback;
    void* User;
    const uint32_t* Pcs;
    size_t Count;
    const Oot3dAotBlockEntryFilter* Filter;
    NativeA32Memory* Memory;
};
bool Equal(const a32::GuestState& a, const a32::GuestState& b) {
    return a.r == b.r && a.cpsr == b.cpsr && a.fpscr == b.fpscr &&
        a.vfp == b.vfp && a.thread_pointer == b.thread_pointer &&
        a.exclusive_address == b.exclusive_address &&
        a.exclusive_token == b.exclusive_token &&
        a.exclusive_size == b.exclusive_size && a.exclusive_valid == b.exclusive_valid;
}
bool Notifies(const Route& route, uint32_t pc) {
    return route.Callback && (!route.Count ||
        std::binary_search(route.Pcs, route.Pcs + route.Count, pc));
}
struct Guard {
    const Oot3dWholeAotProgramV2* Module;
    bool Observed = false;
};
void RejectObserver(uint32_t pc, a32::GuestState& state, a32::MemoryBus&, void* user) {
    auto& guard = *static_cast<Guard*>(user);
    guard.Observed = true;
    guard.Module->ObservableExit(pc, &state);
}
Oot3dWholeAotFlow RejectExternal(uint32_t pc, Oot3dWholeAotFrame&, NativeA32Memory&) {
    return {Oot3dWholeAotFlowKind::Unsupported, pc, 0};
}
bool CpuRead(void* user, uint32_t address, uint8_t size, uint64_t* result) {
    auto& memory = *static_cast<NativeA32Memory*>(user);
    switch (size) {
    case 1: { uint8_t value; if (!memory.ReadFast(address, &value)) return false; *result = value; return true; }
    case 2: { uint16_t value; if (!memory.ReadFast(address, &value)) return false; *result = value; return true; }
    case 4: { uint32_t value; if (!memory.ReadFast(address, &value)) return false; *result = value; return true; }
    case 8: return memory.ReadFast(address, result);
    default: return false;
    }
}
bool CpuWrite(void* user, uint32_t address, uint8_t size, uint64_t value) {
    auto& memory = *static_cast<NativeA32Memory*>(user);
    if (memory.IsExecutable(address, size)) return false;
    switch (size) {
    case 1: return memory.WriteFast(address, static_cast<uint8_t>(value));
    case 2: return memory.WriteFast(address, static_cast<uint16_t>(value));
    case 4: return memory.WriteFast(address, static_cast<uint32_t>(value));
    case 8: return memory.WriteFast(address, value);
    default: return false;
    }
}
bool CpuExecutable(void* user, uint32_t address) {
    return static_cast<NativeA32Memory*>(user)->IsExecutable(address, 4);
}
void ReplayCpu(const a32::GuestState& input, const Route& route, const std::string& path) {
    using nlohmann::json;
    json report{{"format", "triaevum_cpu_replay_v1"}, {"entry", target},
                {"diagnostic_only", true}, {"trials", json::array()},
                {"limits", {"No host services, exclusive accesses or interior observers.",
                            "Page-table Dynarmic omits AOT write-generation bookkeeping.",
                            "Compilation, cloning and fingerprints are outside timing; equal dirty-page resets are inside each batch.",
                            "All memory is verified at warmup and pair end; changed chunks and architectural state are checked throughout.",
                            "Microseconds and thread cycles are not gameplay FPS."}}};
    report["captured_callback_pc"] = input.r[15];
    DWORD_PTR processMask = 0, systemMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) || !processMask)
        throw std::runtime_error("Cannot determine replay thread placement");
    const auto processor = std::bit_floor(processMask);
    const auto previous = SetThreadAffinityMask(GetCurrentThread(), processor);
    if (!previous) throw std::runtime_error("Cannot pin replay thread");
    struct Placement {
        DWORD_PTR Previous;
        ~Placement() { SetThreadAffinityMask(GetCurrentThread(), Previous); }
    } placement{previous};
    report["logical_processor_mask"] = processor;
    Lifecycle(("cpu_entry_" + std::to_string(target)).c_str());
    const auto original = route.Memory->ContentFingerprint();
    const auto originalGeneration = route.Memory->WriteGeneration();
    NativeA32Memory memory(*route.Memory);
    auto reference = input;
    a32::ExecutionResult referenceExit{};
    Oot3dWholeAotStats referenceStats{};
    uint32_t referenceBlocks = 0;
    Guard guard{baseline};
    const bool executed = baseline->Execute(target, reference, memory, &referenceExit,
        &referenceStats, RejectExternal, 100000, &referenceBlocks,
        route.Callback ? RejectObserver : nullptr, &guard, route.Pcs, route.Count,
        route.Filter, true, input.r[14]);
    Lifecycle("cpu_reference_returned");
    const bool eligible = executed && !input.exclusive_valid && !guard.Observed &&
        !(route.Callback && !route.Count) && referenceExit.pc == input.r[14] &&
        referenceExit.kind == a32::ExitKind::Branch && !referenceStats.MemoryFaults &&
        !referenceStats.UnsupportedExits && !referenceStats.ExternalCalls &&
        memory.AllowsUnobservedAccess();
    report["eligible"] = eligible;
    report["reference_exit"] = static_cast<unsigned>(referenceExit.kind);
    report["reference_blocks"] = referenceBlocks;
    const auto expectedMemory = memory.ContentFingerprint();
    const auto expectedGeneration = memory.WriteGeneration();
    report["reference_memory"] = expectedMemory;
    bool valid = eligible;
    if (eligible) {
        auto pages = std::make_unique<std::array<uint8_t*, 1U << 20>>();
        AotReplayMemoryReset resets(*route.Memory, memory, *pages);
        Lifecycle("cpu_reset_constructed");
        resets.Restore();
        // Certify that page resets cover every changed byte before batching.
        // Writes outside these pages cannot silently qualify a repeated run.
        if (memory.ContentFingerprint() != original)
            throw std::runtime_error("CPU replay cannot restore all changed memory by page");
        report["dirty_reset_bytes"] = resets.ByteCount();
        const unsigned repetitions = std::clamp(262144U / std::max(referenceBlocks, 1U), 64U, 2048U);
        DynarmicReplayConfig config{&memory, CpuRead, CpuWrite, CpuExecutable, nullptr,
            target, input.r[14], route.Callback ? route.Pcs : nullptr, route.Callback ? route.Count : 0};
        void* checked = dynarmic->Create(&config);
        config.PageTable = pages->data();
        void* native = dynarmic->Create(&config);
        if (!checked || !native) {
            dynarmic->Destroy(checked); dynarmic->Destroy(native);
            throw std::runtime_error("Cannot create independent Dynarmic replay");
        }
        struct Lifetime {
            void* Checked; void* Native;
            ~Lifetime() { dynarmic->Destroy(Checked); dynarmic->Destroy(Native); }
        } lifetime{checked, native};
        Lifecycle("cpu_jits_created");
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency)) throw std::runtime_error("QPC unavailable");
        // Warm each engine on the captured input. Then compare each with AOT in
        // its own ABBA pairs; the A/A pair reports this capture's noise floor.
        const unsigned pairs = candidate == baseline ? 3 : 4;
        for (unsigned pair = 0; pair < pairs && valid; ++pair) {
            for (int trial = -1; trial < 16; ++trial) {
                const auto trialGeneration = memory.WriteGeneration();
                a32::GuestState state{};
                const bool alternative = trial < 0 || trial % 4 == 1 || trial % 4 == 2;
                const bool useJit = alternative && (pair == 1 || pair == 2);
                const auto* module = alternative && pair == 3 ? candidate : baseline;
                DynarmicReplayState jitState{};
                const unsigned count = trial < 0 ? 1 : repetitions;
                DynarmicReplayResult jitResult{};
                a32::ExecutionResult exit{};
                Oot3dWholeAotStats stats{};
                uint32_t blocks = 0;
                Guard observed{module};
                LARGE_INTEGER begin{}, end{};
                ULONG64 cyclesBegin = 0, cyclesEnd = 0;
                bool clocks = QueryThreadCycleTime(GetCurrentThread(), &cyclesBegin) != 0;
                QueryPerformanceCounter(&begin);
                bool ran = true;
                bool stateMatch = true;
                for (unsigned repeat = 0; repeat < count; ++repeat) {
                    resets.Restore();
                    state = input;
                    if (useJit) {
                        std::copy(state.r.begin(), state.r.end(), jitState.Registers);
                        // AOT materializes its PC lazily; the callback's
                        // explicit target is the actual invocation entry.
                        jitState.Registers[15] = target;
                        std::copy(state.vfp.begin(), state.vfp.end(), jitState.Vfp);
                        jitState.Cpsr = state.cpsr; jitState.Fpscr = state.fpscr;
                        jitResult = dynarmic->Execute(pair == 1 ? checked : native, &jitState, 10000000);
                        std::copy_n(jitState.Registers, 16, state.r.begin());
                        std::copy_n(jitState.Vfp, 32, state.vfp.begin());
                        state.cpsr = jitState.Cpsr; state.fpscr = jitState.Fpscr;
                        ran = jitResult.Exit == DynarmicReplayExit::Returned;
                    } else {
                        stats = {}; blocks = 0;
                        ran = module->Execute(target, state, memory, &exit, &stats,
                            RejectExternal, 100000, &blocks, route.Callback ? RejectObserver : nullptr,
                            &observed, route.Pcs, route.Count, route.Filter, true, input.r[14]);
                        ran = ran && !observed.Observed && !stats.MemoryFaults &&
                            !stats.UnsupportedExits && !stats.ExternalCalls &&
                            exit.kind == referenceExit.kind && exit.pc == referenceExit.pc &&
                            exit.detail == referenceExit.detail && blocks == referenceBlocks;
                    }
                    stateMatch = Equal(state, reference);
                    if (!ran || !stateMatch) break;
                }
                QueryPerformanceCounter(&end);
                clocks = QueryThreadCycleTime(GetCurrentThread(), &cyclesEnd) != 0 && clocks;
                const bool fullMemoryCheck = trial < 0 || trial == 15;
                const auto fingerprint = fullMemoryCheck ? memory.ContentFingerprint() : 0;
                const auto expectedBatchGeneration = trialGeneration +
                    (expectedGeneration - originalGeneration) * count;
                const bool match = stateMatch && resets.MatchesOutput() &&
                    (!fullMemoryCheck || fingerprint == expectedMemory) &&
                    (useJit && pair == 2 || memory.WriteGeneration() == expectedBatchGeneration);
                json row{{"pair", pair}, {"trial", trial}, {"alternative", alternative},
                    {"engine", useJit ? (pair == 1 ? "dynarmic_checked" : "dynarmic_page_table") :
                        (alternative && pair == 3 ? "aot_candidate" : "aot")},
                    {"repetitions", count},
                    {"nanoseconds", (end.QuadPart - begin.QuadPart) * 1e9 / frequency.QuadPart / count},
                    {"thread_cycles", double(cyclesEnd - cyclesBegin) / count}, {"valid", ran && clocks},
                    {"matched", match}, {"memory", fullMemoryCheck ? json(fingerprint) : json(nullptr)},
                    {"full_memory_check", fullMemoryCheck}, {"generation", memory.WriteGeneration()},
                    {"cpsr", state.cpsr}, {"fpscr", state.fpscr},
                    {"instructions", jitResult.Instructions},
                    {"jit_exit", static_cast<unsigned>(jitResult.Exit)}, {"jit_detail", jitResult.Detail}};
                if (!match) {
                    row["expected_cpsr"] = reference.cpsr; row["expected_fpscr"] = reference.fpscr;
                    row["expected_generation"] = expectedBatchGeneration;
                    row["registers"] = state.r; row["expected_registers"] = reference.r;
                    row["vfp"] = state.vfp; row["expected_vfp"] = reference.vfp;
                }
                report["trials"].push_back(std::move(row));
                valid = valid && ran && clocks && match;
                if (!valid) break;
            }
        }
        Lifecycle("cpu_trials_complete");
    }
    const bool untouched = original == route.Memory->ContentFingerprint() &&
        originalGeneration == route.Memory->WriteGeneration();
    report["matched"] = valid;
    report["original_memory_untouched"] = untouched;
    std::ofstream out(path + ".cpu.json");
    out << report.dump(2);
    if (!out) throw std::runtime_error("Cannot write CPU comparison");
    if (!untouched) throw std::runtime_error("CPU comparison changed live memory");
}
void Replay(a32::GuestState input, const Route& route) {
    const char* path = std::getenv("TRIAEVUM_INVOCATION_OUTPUT");
    if (!path) return;
    std::string outputPath(path);
    if (!batchTargets.empty()) outputPath += "." + std::to_string(target) + ".csv";
    if (dynarmic) {
        try { ReplayCpu(input, route, outputPath); }
        catch (const std::exception& error) {
            std::ofstream(outputPath + ".cpu.json") << nlohmann::json{
                {"entry", target}, {"diagnostic_error", error.what()}, {"matched", false}}.dump(2);
            throw;
        }
        return;
    }
    std::ofstream out(outputPath, std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot write invocation diagnostic");
    out << "diagnostic_only,not_game_timing\n";
    out << "original_callback," << (route.Callback != nullptr) << ",hook_count," << route.Count << '\n';
    out << "original_hook_pcs";
    for (size_t i = 0; i < route.Count; ++i) out << ',' << route.Pcs[i];
    out << "\ntrial,variant,nanoseconds,thread_cycles,valid,matched,blocks,return_pc\n";
    auto originalMemory = route.Memory->ContentFingerprint();
    auto originalGeneration = route.Memory->WriteGeneration();
    NativeA32Memory memory(*route.Memory);
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency)) throw std::runtime_error("QPC unavailable");
    a32::GuestState expected{};
    a32::ExecutionResult expectedExit{};
    uint64_t expectedMemory = 0, expectedGeneration = 0;
    uint32_t expectedBlocks = 0;
    bool allValid = true;
    using HitCounter = uint64_t (*)() noexcept;
    HMODULE candidateModule = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(candidate->Execute), &candidateModule);
    auto hitCounter = candidateModule ? reinterpret_cast<HitCounter>(
        GetProcAddress(candidateModule, "triaevum_invocation_candidate_hits")) : nullptr;
    const auto hitsBefore = hitCounter ? hitCounter() : 0;
    auto leafCounter = candidateModule ? reinterpret_cast<HitCounter>(
        GetProcAddress(candidateModule,"triaevum_invocation_native_leaf_hits")) : nullptr;
    const auto leavesBefore = leafCounter ? leafCounter() : 0;
    using CoverageEntry = uint32_t (*)(uint32_t) noexcept;
    using CoverageHits = uint64_t (*)(uint32_t) noexcept;
    auto coverageEntry=candidateModule?reinterpret_cast<CoverageEntry>(
        GetProcAddress(candidateModule,"triaevum_invocation_coverage_entry")):nullptr;
    auto coverageHits=candidateModule?reinterpret_cast<CoverageHits>(
        GetProcAddress(candidateModule,"triaevum_invocation_coverage_hits")):nullptr;
    std::vector<uint64_t> visitsBefore;
    if(coverageEntry && coverageHits)
        for(uint32_t i=0; i<10000 && coverageEntry(i); ++i) visitsBefore.push_back(coverageHits(i));
    // Batch mode qualifies real inputs, never performance: one A/B pair per root.
    const unsigned trials=batchTargets.empty()?16:2;
    for (unsigned trial = 0; trial < trials; ++trial) {
        memory = *route.Memory; // Copy/reset and fingerprints are outside timing.
        auto state = input;
        Oot3dWholeAotStats stats{};
        a32::ExecutionResult result{};
        uint32_t blocks = 0;
        bool useCandidate = trial % 4 == 1 || trial % 4 == 2;
        const auto* module = useCandidate ? candidate : baseline;
        Guard guard{module};
        LARGE_INTEGER begin{}, end{};
        ULONG64 cyclesBegin = 0, cyclesEnd = 0;
        bool clocks = QueryThreadCycleTime(GetCurrentThread(), &cyclesBegin) != 0;
        QueryPerformanceCounter(&begin);
        bool executed = module->Execute(target, state, memory, &result, &stats,
            RejectExternal, 100000, &blocks, route.Callback ? RejectObserver : nullptr,
            &guard, route.Pcs, route.Count, route.Filter, true, input.r[14]);
        QueryPerformanceCounter(&end);
        clocks = QueryThreadCycleTime(GetCurrentThread(), &cyclesEnd) != 0 && clocks;
        const auto fingerprint = memory.ContentFingerprint();
        const auto generation = memory.WriteGeneration();
        bool valid = clocks && executed && !guard.Observed &&
            result.pc == input.r[14] && result.kind == a32::ExitKind::Branch &&
            stats.MemoryFaults == 0 && stats.UnsupportedExits == 0 && stats.ExternalCalls == 0;
        if (!trial) {
            expected = state; expectedExit = result; expectedMemory = fingerprint;
            expectedGeneration = generation; expectedBlocks = blocks;
        }
        bool match = Equal(expected, state) && fingerprint == expectedMemory &&
            generation == expectedGeneration && blocks == expectedBlocks &&
            result.kind == expectedExit.kind && result.pc == expectedExit.pc &&
            result.detail == expectedExit.detail && result.fallback == expectedExit.fallback;
        out << trial << ',' << (useCandidate ? "candidate" : "baseline") << ','
            << (end.QuadPart - begin.QuadPart) * 1e9 / frequency.QuadPart << ','
            << cyclesEnd - cyclesBegin << ',' << valid << ',' << match << ','
            << blocks << ',' << result.pc << '\n';
        out << "exit_detail," << trial << ',' << static_cast<unsigned>(result.kind) << ','
            << result.detail << ',' << stats.MemoryFaults << ',' << stats.UnsupportedExits << ','
            << stats.ExternalCalls << ',' << guard.Observed << '\n';
        allValid = allValid && valid && match;
        if (!allValid) break;
    }
    const bool untouched = originalMemory == route.Memory->ContentFingerprint() &&
        originalGeneration == route.Memory->WriteGeneration();
    if (hitCounter) out << "candidate_fast_hits," << hitCounter() - hitsBefore << '\n';
    if (leafCounter) out << "candidate_native_leaf_hits," << leafCounter() - leavesBefore << '\n';
    for(uint32_t i=0;i<visitsBefore.size();++i)
        if(coverageHits(i)>visitsBefore[i])
            out << "visited_function," << coverageEntry(i) << ',' << coverageHits(i)-visitsBefore[i] << '\n';
    out << "summary," << allValid << ",original_memory_untouched," << untouched << '\n';
    out.flush();
    if (!untouched) throw std::runtime_error("Invocation probe changed live memory");
}
void Observe(uint32_t pc, a32::GuestState& state, a32::MemoryBus& bus, void* user) {
    auto& route = *static_cast<Route*>(user);
    if (Notifies(route, pc)) route.Callback(pc, state, bus, route.User);
    if (!batchTargets.empty()) {
        for(auto& item:batchTargets) {
            if(item.Pc!=pc || item.Done || ++item.Seen!=captureOccurrence) continue;
            item.Done=true;
            target=pc;
            Replay(state,route);
            completed=std::all_of(batchTargets.begin(),batchTargets.end(),[](const auto& t){return t.Done;});
            break;
        }
        return;
    }
    if (!completed && pc == target && (!inputSelector || inputSelector(&state,route.Memory)) &&
        ++occurrences == captureOccurrence) {
        completed = true;
        Replay(state, route);
    }
}
bool Execute(uint32_t pc, a32::GuestState& state, NativeA32Memory& memory,
    a32::ExecutionResult* result, Oot3dWholeAotStats* stats,
    Oot3dWholeAotExternalCall external, uint32_t budget, uint32_t* consumed,
    a32::BlockEntryCallback callback, void* user, const uint32_t* pcs, size_t count,
    const Oot3dAotBlockEntryFilter* filter, bool skipFirst, uint32_t stop) {
    static bool entered=false;
    if(!entered) { entered=true; Lifecycle("first_execute"); }
    if (completed) return baseline->Execute(pc, state, memory, result, stats, external,
        budget, consumed, callback, user, pcs, count, filter, skipFirst, stop);
    Route route{callback, user, pcs, count, filter, &memory};
    std::vector<uint32_t> hooks;
    if (count) hooks.assign(pcs, pcs + count);
    if(batchTargets.empty()) hooks.push_back(target);
    else for(const auto& item:batchTargets) if(!item.Done) hooks.push_back(item.Pc);
    std::sort(hooks.begin(), hooks.end());
    hooks.erase(std::unique(hooks.begin(), hooks.end()), hooks.end());
    Oot3dAotBlockEntryFilter merged;
    for (auto hook : hooks) merged.Insert(hook);
    // An existing unfiltered callback must remain unfiltered.
    const bool all = callback && !count;
    return baseline->Execute(pc, state, memory, result, stats, external, budget,
        consumed, Observe, &route, all ? nullptr : hooks.data(), all ? 0 : hooks.size(),
        all ? nullptr : &merged, skipFirst, stop);
}
const Oot3dWholeAotProgramV2* Load(const char* path) {
    if (!path) return nullptr;
    auto library = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!library) return nullptr;
    using Query = const Oot3dWholeAotProgramV2* (*)(uint32_t) noexcept;
    auto query = reinterpret_cast<Query>(GetProcAddress(library, "triaevum_title_whole_aot_query"));
    auto result = query ? query(kOot3dWholeAotPluginAbiV2) : nullptr;
    if (!result || result->StructSize != sizeof(*result) || !result->Execute ||
        !result->ObservableExit ||
        result->MemoryWriteObservationVersion != NativeA32Memory::WriteObservationAbiVersion()) return nullptr;
    return result; // Keep loaded for the lifetime of the diagnostic process.
}
}
extern "C" __declspec(dllexport) const Oot3dWholeAotProgramV2*
triaevum_title_whole_aot_query(uint32_t abi) noexcept {
    if (abi != kOot3dWholeAotPluginAbiV2) return nullptr;
    static Oot3dWholeAotProgramV2 proxy{};
    if (!proxy.Execute) {
        Lifecycle("query_begin");
        baseline = Load(std::getenv("TRIAEVUM_INVOCATION_BASELINE"));
        const char* other = std::getenv("TRIAEVUM_INVOCATION_CANDIDATE");
        candidate = other ? Load(other) : baseline;
        if (candidate) {
            HMODULE module=nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(candidate->Execute), &module);
            inputSelector=module ? reinterpret_cast<InputSelector>(
                GetProcAddress(module,"triaevum_invocation_select_input")) : nullptr;
        }
        const char* entry = std::getenv("TRIAEVUM_INVOCATION_ENTRY");
        if (!baseline || !candidate || !entry) return nullptr;
        if (const char* bridge = std::getenv("TRIAEVUM_DYNARMIC_REPLAY")) {
            auto library = LoadLibraryExA(bridge, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            auto query = library ? reinterpret_cast<DynarmicReplayQuery>(
                GetProcAddress(library, "triaevum_dynarmic_replay_api")) : nullptr;
            dynarmic = query ? query() : nullptr;
            if (!dynarmic || dynarmic->Version != 1) return nullptr;
        }
        target = static_cast<uint32_t>(std::strtoul(entry, nullptr, 16));
        if(const char* targets=std::getenv("TRIAEVUM_INVOCATION_TARGETS")) {
            std::istringstream stream(targets);
            std::string value;
            while(std::getline(stream,value,',')) {
                const auto pc=static_cast<uint32_t>(std::strtoul(value.c_str(),nullptr,16));
                if(!pc || pc%4 || batchTargets.size()>=1024) return nullptr;
                batchTargets.push_back({pc});
            }
        }
        if (const char* occurrence = std::getenv("TRIAEVUM_INVOCATION_OCCURRENCE")) {
            auto requested = std::strtoul(occurrence, nullptr, 10);
            if (!requested || requested > 8192) return nullptr;
            captureOccurrence = static_cast<unsigned>(requested);
        }
        proxy = *baseline;
        proxy.Execute = Execute;
        Lifecycle("query_ready");
    }
    return &proxy;
}
