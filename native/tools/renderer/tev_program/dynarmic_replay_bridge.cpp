// Developer-only adapter to an independently built Dynarmic. Not a product
// execution backend; no donor source or ROM-derived data is copied here.
#include "dynarmic_replay_bridge.h"
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/ir/terminal.h>
#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace {
class Replay final : public Dynarmic::A32::UserCallbacks {
public:
    explicit Replay(const DynarmicReplayConfig& input) : config(input) {
        if (input.ObserverCount)
            observers.assign(input.Observers, input.Observers + input.ObserverCount);
        std::sort(observers.begin(), observers.end());
        Dynarmic::A32::UserConfig options;
        options.callbacks = this;
        options.arch_version = Dynarmic::A32::ArchVersion::v6K;
        options.page_table = reinterpret_cast<std::array<uint8_t*, 1U << 20>*>(input.PageTable);
        options.detect_misaligned_access_via_page_table = 8 | 16 | 32 | 64;
        options.only_detect_misalignment_via_page_table_on_page_boundary = true;
        options.check_halt_on_memory_access = true;
        options.code_cache_size = 16 * 1024 * 1024;
        jit = std::make_unique<Dynarmic::A32::Jit>(options);
    }
    DynarmicReplayResult Execute(DynarmicReplayState& state, uint64_t budget) {
        result = {DynarmicReplayExit::Budget, 0, 0};
        remaining = budget;
        jit->Reset(); // Does not discard compiled blocks.
        std::copy_n(state.Registers, 16, jit->Regs().begin());
        std::copy_n(state.Vfp, 32, jit->ExtRegs().begin());
        jit->SetCpsr(state.Cpsr);
        jit->SetFpscr(state.Fpscr);
        jit->Run();
        std::copy_n(jit->Regs().begin(), 16, state.Registers);
        std::copy_n(jit->ExtRegs().begin(), 32, state.Vfp);
        state.Cpsr = jit->Cpsr();
        state.Fpscr = jit->Fpscr();
        return result;
    }
private:
    void Stop(DynarmicReplayExit reason, uint32_t detail) {
        if (result.Exit == DynarmicReplayExit::Budget) result = {reason, detail, result.Instructions};
        if (jit) jit->HaltExecution();
    }
    template<class T> T Read(uint32_t address) {
        uint64_t value = 0;
        if (!config.Read(config.User, address, sizeof(T), &value))
            Stop(DynarmicReplayExit::MemoryFault, address);
        return static_cast<T>(value);
    }
    template<class T> void Write(uint32_t address, T value) {
        if (!config.Write(config.User, address, sizeof(T), value))
            Stop(DynarmicReplayExit::MemoryFault, address);
    }
    std::optional<uint32_t> MemoryReadCode(uint32_t address) override {
        uint64_t value = 0;
        if (!config.Executable(config.User, address) ||
            !config.Read(config.User, address, 4, &value)) return std::nullopt;
        return static_cast<uint32_t>(value);
    }
    bool PreCodeReadHook(bool thumb, uint32_t pc, Dynarmic::A32::IREmitter& ir) override {
        // Stop before executing the caller continuation, without patching guest
        // code. Other observers remain real exclusion boundaries.
        const bool observer = pc != config.Entry &&
            std::binary_search(observers.begin(), observers.end(), pc);
        auto instruction = MemoryReadCode(pc);
        // Exclusive/swap and system coprocessor operations cannot be privately
        // replayed without their owner. Reject them instead of inventing state.
        const bool system = instruction &&
            (((*instruction & 0x0F000010U) == 0x0E000010U &&
              ((*instruction >> 8) & 15U) != 10U && ((*instruction >> 8) & 15U) != 11U) ||
             ((*instruction & 0x0F0000F0U) == 0x01000090U));
        if (pc == config.Stop || observer || thumb || system) {
            ir.BranchWritePC(ir.Imm32(pc));
            ir.ExceptionRaised(Dynarmic::A32::Exception::Breakpoint);
            ir.SetTerm(Dynarmic::IR::Term::CheckHalt{Dynarmic::IR::Term::ReturnToDispatch{}});
            return false;
        }
        return true;
    }
    uint8_t MemoryRead8(uint32_t a) override { return Read<uint8_t>(a); }
    uint16_t MemoryRead16(uint32_t a) override { return Read<uint16_t>(a); }
    uint32_t MemoryRead32(uint32_t a) override { return Read<uint32_t>(a); }
    uint64_t MemoryRead64(uint32_t a) override { return Read<uint64_t>(a); }
    void MemoryWrite8(uint32_t a, uint8_t v) override { Write(a, v); }
    void MemoryWrite16(uint32_t a, uint16_t v) override { Write(a, v); }
    void MemoryWrite32(uint32_t a, uint32_t v) override { Write(a, v); }
    void MemoryWrite64(uint32_t a, uint64_t v) override { Write(a, v); }
    void InterpreterFallback(uint32_t pc, size_t) override {
        Stop(DynarmicReplayExit::Unsupported, pc);
    }
    void CallSVC(uint32_t swi) override { Stop(DynarmicReplayExit::Unsupported, swi); }
    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception exception) override {
        if (exception == Dynarmic::A32::Exception::Breakpoint && pc == config.Stop)
            Stop(DynarmicReplayExit::Returned, pc);
        else if (pc != config.Entry && std::binary_search(observers.begin(), observers.end(), pc))
            Stop(DynarmicReplayExit::Observer, pc);
        else Stop(DynarmicReplayExit::Unsupported, pc);
    }
    void AddTicks(uint64_t ticks) override {
        result.Instructions += ticks;
        remaining = ticks >= remaining ? 0 : remaining - ticks;
    }
    uint64_t GetTicksRemaining() override { return remaining; }
    DynarmicReplayConfig config;
    std::vector<uint32_t> observers;
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    DynarmicReplayResult result{};
    uint64_t remaining = 0;
};
void* Create(const DynarmicReplayConfig* config) noexcept {
    if (!config || !config->Read || !config->Write || !config->Executable ||
        !config->Entry || !config->Stop || (config->Entry | config->Stop) % 4 ||
        (config->ObserverCount && !config->Observers)) return nullptr;
    try { return new Replay(*config); } catch (...) { return nullptr; }
}
void Destroy(void* replay) noexcept { delete static_cast<Replay*>(replay); }
DynarmicReplayResult Execute(void* replay, DynarmicReplayState* state, uint64_t budget) noexcept {
    if (!replay || !state || !budget || (state->Cpsr & 0x20U))
        return {DynarmicReplayExit::Invalid, 0, 0};
    try { return static_cast<Replay*>(replay)->Execute(*state, budget); }
    catch (...) { return {DynarmicReplayExit::Invalid, 0, 0}; }
}
}
extern "C" __declspec(dllexport) const DynarmicReplayApi* triaevum_dynarmic_replay_api() noexcept {
    static const DynarmicReplayApi api{1, Create, Destroy, Execute};
    return &api;
}
