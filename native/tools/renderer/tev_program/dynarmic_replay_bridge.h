#pragma once

// Developer-only POD boundary: Azahar's /MD libraries never exchange C++
// containers or allocations with the title replay's /MT runtime.
#include <cstddef>
#include <cstdint>

struct DynarmicReplayState {
    uint32_t Registers[16];
    uint32_t Vfp[32];
    uint32_t Cpsr;
    uint32_t Fpscr;
};

struct DynarmicReplayConfig {
    void* User;
    bool (*Read)(void*, uint32_t, uint8_t, uint64_t*);
    bool (*Write)(void*, uint32_t, uint8_t, uint64_t);
    bool (*Executable)(void*, uint32_t);
    uint8_t** PageTable; // Optional 2^20 entries; null entries use callbacks.
    uint32_t Entry;
    uint32_t Stop;
    const uint32_t* Observers;
    size_t ObserverCount;
};

enum class DynarmicReplayExit : uint32_t {
    Returned, Budget, MemoryFault, Observer, Unsupported, Invalid
};
struct DynarmicReplayResult {
    DynarmicReplayExit Exit;
    uint32_t Detail;
    uint64_t Instructions;
};
struct DynarmicReplayApi {
    uint32_t Version;
    void* (*Create)(const DynarmicReplayConfig*) noexcept;
    void (*Destroy)(void*) noexcept;
    DynarmicReplayResult (*Execute)(void*, DynarmicReplayState*, uint64_t) noexcept;
};
using DynarmicReplayQuery = const DynarmicReplayApi* (*)() noexcept;
