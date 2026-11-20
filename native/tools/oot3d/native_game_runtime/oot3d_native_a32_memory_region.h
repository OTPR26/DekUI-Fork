#pragma once

#include "oot3d_native_a32_memory.h"
#include "oot3d_native_whole_aot_runtime.h"

#if defined(_MSC_VER)
#define OOT3D_REGION_INLINE __forceinline
#elif defined(__GNUC__)
#define OOT3D_REGION_INLINE inline __attribute__((always_inline))
#else
#define OOT3D_REGION_INLINE inline
#endif

namespace Oot3dNativeGame {

struct NativeA32RegionCounters {
    uint64_t ReadHits = 0, WriteHits = 0, Resolutions = 0, CheckedAccesses = 0;
    uint64_t CopyChunks = 0, CopyIterations = 0;
    uint64_t Observed = 0, Budget = 0, Mapping = 0, Overlap = 0;
};

// Scoped to synchronous, callback-free execution. No pointer escapes; no map,
// service, trace observer or exclusive operation may run inside this scope.
// Failed range qualification falls back BEFORE modifying any guest state.
class NativeA32UnobservedMemory final {
    static_assert(std::endian::native == std::endian::little);
  public:
    explicit NativeA32UnobservedMemory(NativeA32Memory& memory,
                                      NativeA32RegionCounters* counters = nullptr) noexcept
        : mMemory(memory), mCounters(counters), mEnabled(Eligible(memory)) {}
    NativeA32UnobservedMemory(const NativeA32UnobservedMemory&) = delete;
    NativeA32UnobservedMemory& operator=(const NativeA32UnobservedMemory&) = delete;
    ~NativeA32UnobservedMemory() noexcept { Commit(); }

    static bool Eligible(const NativeA32Memory& memory) noexcept {
        return memory.AllowsUnobservedAccess();
    }

    void Commit() noexcept {
        mMemory.mWriteGeneration += mWrites;
        mWrites = 0;
    }

    template <typename T>
    OOT3D_REGION_INLINE bool ReadFast(uint32_t address, T* value, uint32_t* fault = nullptr) noexcept {
        static_assert(std::is_unsigned_v<T> && std::is_integral_v<T>);
        if (!value) return false;
        if (mEnabled) {
            if (const auto* bytes = Resolve(address, sizeof(T), false)) {
                std::memcpy(value, bytes, sizeof(T));
                return true;
            }
        }
        Commit();
        if (mCounters) ++mCounters->CheckedAccesses;
        return mMemory.ReadFast<T>(address, value, fault);
    }

    template <typename T>
    OOT3D_REGION_INLINE bool WriteFast(uint32_t address, T value, uint32_t* fault = nullptr) noexcept {
        static_assert(std::is_unsigned_v<T> && std::is_integral_v<T>);
        if (mEnabled) {
            if (auto* bytes = Resolve(address, sizeof(T), true)) {
                std::memcpy(bytes, &value, sizeof(T));
                ++mWrites;
                mMemory.MarkRangeWritten(address, sizeof(T),
                    mMemory.CurrentRangeGeneration() + mWrites);
                return true;
            }
        }
        Commit();
        if (mCounters) ++mCounters->CheckedAccesses;
        return mMemory.WriteFast<T>(address, value, fault);
    }

    // A proven, nonaliasing carried-word copy loop. Validate its lookahead as
    // well as the stores; retain instruction order in the scalar fallback.
    bool CopyPairs(uint32_t& source, uint32_t& destination, uint32_t& count,
                   uint32_t& carried, uint32_t& scratch,
                   Oot3dAotLazyFlags& flags, Oot3dWholeAotContext& context,
                   uint32_t pc, Oot3dAotFlagMask live = Oot3dAotFlagMask::Nzcv) noexcept {
        if (!Eligible(mMemory) || Observed(context, pc, pc)) {
            if (mCounters) ++mCounters->Observed;
            return false;
        }
        const uint32_t iterations = std::min(count, context.BlocksRemaining);
        if (iterations < 2U) {
            if (mCounters) ++mCounters->Budget;
            return false;
        }
        const uint64_t length = uint64_t{iterations} * 8U;
        const uint64_t readBegin = uint64_t{source} + 8U;
        const uint64_t writeBegin = uint64_t{destination} + 4U;
        const uint64_t readEnd = readBegin + length;
        const uint64_t writeEnd = writeBegin + length;
        if (readEnd > (uint64_t{1} << 32U) || writeEnd > (uint64_t{1} << 32U)) {
            if (mCounters) ++mCounters->Mapping;
            return false;
        }
        if (readBegin < writeEnd && writeBegin < readEnd) {
            if (mCounters) ++mCounters->Overlap;
            return false;
        }
        auto* input = Resolve(static_cast<uint32_t>(readBegin), size_t(length), false);
        auto* output = Resolve(static_cast<uint32_t>(writeBegin), size_t(length), true);
        if (!input || !output) {
            if (mCounters) ++mCounters->Mapping;
            return false;
        }
        // The first store uses the previous iteration's carried word. Remaining
        // stores are contiguous source words; final registers include lookahead.
        uint32_t lastScratch, lastCarry;
        std::memcpy(&lastScratch, input + length - 8U, 4U);
        std::memcpy(&lastCarry, input + length - 4U, 4U);
        std::memcpy(output, &carried, 4U);
        std::memcpy(output + 4U, input, size_t(length - 4U));
        source += static_cast<uint32_t>(length);
        destination += static_cast<uint32_t>(length);
        flags.SetSubtract(count - iterations + 1U, 1U, live);
        count -= iterations;
        carried = lastCarry;
        scratch = lastScratch;
        context.BlocksRemaining -= iterations;
        context.BlocksConsumed += iterations;
        mWrites += uint64_t{iterations} * 2U;
        mMemory.MarkRangeWritten(static_cast<uint32_t>(writeBegin), size_t(length),
            mMemory.CurrentRangeGeneration() + mWrites);
        if (mCounters) {
            ++mCounters->CopyChunks;
            mCounters->CopyIterations += iterations;
        }
        return true;
    }

    static bool Observed(const Oot3dWholeAotContext& context,
                         uint32_t first, uint32_t last) noexcept {
        if (!context.BlockEntry) return false;
        if (!context.BlockEntryPcCount) return true;
        const auto* end = context.BlockEntryPcs + context.BlockEntryPcCount;
        const auto* hook = std::lower_bound(context.BlockEntryPcs, end, first);
        return hook != end && *hook <= last;
    }

  private:
    struct View {
        uint64_t Begin = 0, End = 0;
        uint8_t* Bytes = nullptr;
        bool Writable = false;
        bool Contains(uint32_t address, size_t size, bool write) const noexcept {
            return Bytes && address >= Begin && address <= End && size <= End - address &&
                   (!write || Writable);
        }
    };

    OOT3D_REGION_INLINE uint8_t* Resolve(uint32_t address, size_t size, bool write) noexcept {
        auto& view = write ? mWriteView : mReadView;
        if (view.Contains(address, size, write)) {
            if (mCounters) {
                if (write) ++mCounters->WriteHits;
                else ++mCounters->ReadHits;
            }
            return view.Bytes + (uint64_t{address} - view.Begin);
        }
        if (mCounters) ++mCounters->Resolutions;
        auto* region = mMemory.FindRegion(address, size);
        if (!region || (write && !region->Writable)) return nullptr;
        view = {region->BaseAddress, uint64_t{region->BaseAddress} + region->Bytes.size(),
                region->Bytes.data(), region->Writable};
        return view.Bytes + (uint64_t{address} - view.Begin);
    }

    NativeA32Memory& mMemory;
    NativeA32RegionCounters* mCounters;
    const bool mEnabled;
    View mReadView{}, mWriteView{};
    uint64_t mWrites = 0;
};

} // namespace Oot3dNativeGame

#undef OOT3D_REGION_INLINE
