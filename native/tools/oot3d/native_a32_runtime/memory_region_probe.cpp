#include "oot3d_native_a32_memory_region.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace Oot3dNativeGame;
namespace a32 = oot3d::recomp::a32;
using Function = Oot3dWholeAotFlow (*)(Oot3dWholeAotFrame&, Oot3dWholeAotContext&,
                                    Oot3dAotArchitecturalState&, uint32_t);
struct TestFunction { uint32_t Entry; Function Baseline, Candidate; };
#include "memory_region_fixture.inc"

struct Result {
    a32::GuestState Guest{};
    Oot3dWholeAotFlow Flow{};
    uint32_t Remaining{}, Consumed{};
    uint64_t Memory{}, Trace{}, Generation{};
    std::vector<uint32_t> Hooks;
};

static void Callback(uint32_t pc, a32::GuestState& guest, a32::MemoryBus&, void* user) {
    static_cast<std::vector<uint32_t>*>(user)->push_back(pc);
    guest.cpsr ^= a32::kFlagV;
}

static Result Run(const TestFunction& function, bool candidate, unsigned budget,
                  unsigned mode, unsigned fault, bool trace, uint32_t seed, bool resume) {
    Result result;
    NativeA32Memory memory;
    std::vector<uint8_t> bytes(0x40000);
    uint32_t random = seed | 1U;
    for (auto& byte : bytes) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        byte = static_cast<uint8_t>(random);
    }
    if (!memory.MapRegion({"ram", 0x10000, bytes.size(), fault != 3, false, bytes})) std::abort();
    const auto write = [&](uint32_t address, uint32_t value) { memory.WriteHost<uint32_t>(address, value); };
    result.Guest.r[13] = fault == 2 ? 0x10010 : 0x4F000;
    result.Guest.r[14] = 0x9000;
    result.Guest.cpsr = 0xA8000010U;
    if (function.Entry == 0x1000) {
        result.Guest.r[0] = fault == 1 ? 0x4FFF0 : 0x14000;
        result.Guest.r[1] = fault == 4 ? 0x14008 : (fault == 5 ? 0xFFFFFFF8 : 0x16000);
        result.Guest.r[2] = seed % 17;
        result.Guest.r[3] = random;
    } else {
        result.Guest.r[0] = fault == 1 ? 0x50000 : 0x11000;
        result.Guest.r[1] = 0x11200;
        for (uint32_t channel = 0; channel < 4; ++channel) {
            write(0x11200 + 4 * channel, 0x14000 + channel * 0x1000);
            write(0x1101C + 4 * channel, 0x18000 + channel * 0x1000);
        }
        write(0x11040, 0);
        write(0x1103C, 2);
        memory.WriteHost<uint8_t>(0x11055, 2);
        memory.WriteHost<uint8_t>(0x11056, 1);
        memory.WriteHost<uint8_t>(0x11100, 1);
        for (auto offset : {0x9CU, 0xA4U, 0xB0U, 0xB4U, 0xC4U}) write(0x11000 + offset, seed % 4);
        for (auto offset : {0x98U, 0xA0U, 0xA8U, 0xACU, 0xC0U}) write(0x11000 + offset, 320);
        for (uint32_t channel = 0; channel < 2; ++channel) {
            for (uint32_t i = 0; i < 5; ++i) {
                const std::array<uint32_t, 5> offsets{0x38, 0x48, 0x58, 0x5C, 0x78};
                auto offset = offsets[i] + channel * ((i == 2 || i == 3) ? 8 : 4);
                write(0x11000 + offset, 0x20000 + i * 0x4000 + channel * 0x1000);
            }
        }
        if (fault == 4) write(0x11200, 0x11000); // buffers alias configuration
        if (fault == 5) write(0x11200, 0x4FF00); // partial loads/stores before fault
    }
    memory.EnableWriteTraceFingerprint(trace);
    Oot3dWholeAotStats stats;
    Oot3dWholeAotContext context{memory, memory, stats};
    context.BlocksRemaining = budget;
    const std::array<uint32_t, 1> hook{mode == 2 ? function.Entry : 0x7000};
    if (mode) {
        context.BlockEntry = Callback;
        context.BlockEntryUser = &result.Hooks;
        if (mode > 1) { context.BlockEntryPcs = hook.data(); context.BlockEntryPcCount = hook.size(); }
    }
    Oot3dWholeAotFrame frame(result.Guest);
    Oot3dAotArchitecturalState state(result.Guest);
    uint32_t entry = function.Entry;
    for (unsigned chunk = 0; chunk < (resume ? 8U : 1U); ++chunk) {
        context.BlocksRemaining = budget;
        context.BlocksConsumed = 0;
        result.Flow = (candidate ? function.Candidate : function.Baseline)(frame, context, state, entry);
        if (result.Flow.Kind != Oot3dWholeAotFlowKind::BlockLimit || budget == 0) break;
        entry = result.Flow.Pc;
    }
    state.Flush(0x7FFFU, true);
    result.Remaining = context.BlocksRemaining; result.Consumed = context.BlocksConsumed;
    result.Memory = memory.ContentFingerprint(); result.Trace = memory.WriteTraceFingerprint();
    result.Generation = memory.WriteGeneration();
    return result;
}

int main() {
    unsigned count = 0;
    for (const auto& function : kFunctions)
    for (unsigned budget : {0U, 1U, 2U, 3U, 7U, 16U, 160U, 5000U})
    for (unsigned mode = 0; mode < 4; ++mode)
    for (unsigned fault = 0; fault < 6; ++fault)
    for (bool trace : {false, true})
    for (uint32_t seed : {3U, 17U, 31U})
    for (bool resume : {false, true}) {
        const auto a = Run(function, false, budget, mode, fault, trace, seed, resume);
        const auto b = Run(function, true, budget, mode, fault, trace, seed, resume);
        if (a.Guest.r != b.Guest.r || a.Guest.cpsr != b.Guest.cpsr ||
            a.Flow.Kind != b.Flow.Kind || a.Flow.Pc != b.Flow.Pc || a.Flow.Detail != b.Flow.Detail ||
            a.Remaining != b.Remaining || a.Consumed != b.Consumed || a.Memory != b.Memory ||
            a.Trace != b.Trace || a.Generation != b.Generation || a.Hooks != b.Hooks) {
            std::fprintf(stderr, "Mismatch entry=%x budget=%u mode=%u fault=%u trace=%u seed=%u resume=%u\n",
                         function.Entry, budget, mode, fault, trace, seed, resume);
            return 1;
        }
        ++count;
    }
    // The view preserves exact store counts, including mixed checked accesses.
    NativeA32Memory memory;
    memory.MapRegion({"ram", 0x1000, 4096, true, false, {}});
    NativeA32RegionCounters counters;
    auto generation = memory.WriteGeneration();
    {
        NativeA32UnobservedMemory region(memory, &counters);
        for (uint32_t i = 0; i < 10; ++i) {
            uint32_t value;
            if (!region.WriteFast(0x1000 + i * 4, i) || !region.ReadFast(0x1000 + i * 4, &value) || value != i)
                return 2;
        }
    }
    if (memory.WriteGeneration() != generation + 10 || counters.WriteHits != 9 || counters.ReadHits != 9)
        return 3;
    {
        NativeA32UnobservedMemory region(memory, &counters);
        Oot3dWholeAotStats stats;
        Oot3dWholeAotContext context{memory, memory, stats};
        context.BlocksRemaining = 20;
        uint32_t source = 0x1000, destination = 0x1800, count = 4, carry = 9, scratch = 0;
        Oot3dAotLazyFlags flags(0);
        if (!region.CopyPairs(source, destination, count, carry, scratch, flags, context, 0x8000) ||
            counters.CopyChunks != 1 || counters.CopyIterations != 4 || count != 0 ||
            context.BlocksRemaining != 16) return 4;
        count = 4;
        destination = source;
        if (region.CopyPairs(source, destination, count, carry, scratch, flags, context, 0x8000) ||
            counters.Overlap != 1) return 5;
        context.BlocksRemaining = 1;
        if (region.CopyPairs(source, destination, count, carry, scratch, flags, context, 0x8000) ||
            counters.Budget != 1) return 6;
    }
    // A mapped allocation may still contain device-owned bytes. Neither the
    // bulk-copy path nor a retained view may bypass their read observation.
    {
        const std::array ranges{std::pair<uint32_t, size_t>{0x1000, 16}};
        size_t fetched = 0;
        if (!memory.DeferWrites(ranges, [&](uint32_t, std::span<uint8_t> output) {
                std::fill(output.begin(), output.end(), 0x55);
                fetched += output.size();
                return true;
            })) return 7;
        NativeA32UnobservedMemory region(memory);
        Oot3dWholeAotStats stats;
        Oot3dWholeAotContext context{memory, memory, stats};
        context.BlocksRemaining = 8;
        uint32_t source = 0x0FF8, destination = 0x1800, copies = 4, carry = 9, scratch = 0;
        Oot3dAotLazyFlags flags(0);
        if (region.CopyPairs(source, destination, copies, carry, scratch, flags, context, 0x8000) ||
            fetched != 0 || copies != 4 || context.BlocksRemaining != 8) return 8;
        uint32_t value = 0;
        if (!region.ReadFast(0x1000, &value) || value != 0x55555555U || fetched == 0) return 9;
    }
    std::printf("Memory-region differential: %u cases passed; view resolution/write counts verified\n", count);
}
