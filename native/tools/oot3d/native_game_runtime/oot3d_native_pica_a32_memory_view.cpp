#include "oot3d_native_pica_submission.h"

#include "oot3d_native_a32_memory.h"
#include <algorithm>

namespace Oot3dNativeGame {

Oot3dPicaPhysicalMemoryView::Oot3dPicaPhysicalMemoryView(
    const NativeA32Memory& memory,
    std::vector<Oot3dPicaPhysicalMemoryRegion> regions)
    : Oot3dPicaPhysicalMemoryView(
          std::move(regions),
          [&memory](uint32_t address, size_t size) {
              return memory.SynchronousReadView(address, size);
          },
          [&memory](uint32_t address, size_t size) {
              return memory.RangeWriteGeneration(address, size);
          },
          [&memory](uint32_t address, size_t size) {
              return memory.IsMapped(address, size);
          }) {}

bool Oot3dPicaPhysicalMemoryView::SynchronizeGuestWrite(
    NativeA32Memory& memory, uint32_t guestAddress, size_t size) const {
    const uint64_t end = uint64_t{guestAddress} + size;
    for (const auto& region : mRegions) {
        const uint64_t low = std::max<uint64_t>(guestAddress, region.GuestBaseAddress);
        const uint64_t high = std::min(end, uint64_t{region.GuestBaseAddress} + region.Size);
        if (low >= high) continue;
        const size_t count = static_cast<size_t>(high - low);
        const uint32_t physical = region.PhysicalBaseAddress +
                                  static_cast<uint32_t>(low - region.GuestBaseAddress);
        const auto source = memory.SynchronousReadView(static_cast<uint32_t>(low), count);
        if (source.size() != count) return false;
        NativeA32Memory::RangeWatchSuppression materialization(memory);
        if (!ForEachGuestAlias(physical, count, [&](uint32_t alias, size_t) {
                return alias == low || memory.WriteHostBytes(alias, source);
            })) return false;
    }
    return true;
}

} // namespace Oot3dNativeGame
