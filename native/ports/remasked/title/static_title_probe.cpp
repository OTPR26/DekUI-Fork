// Checks the actual generated title through the production loader. No boot claim.
#include "oot3d_native_direct_aot.h"
#include "triaevum_title_plugin_loader.h"
#include "oot3d_native_a32_memory.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
int main(int argc, char** argv) {
  using namespace Oot3dNativeGame;
  try { ConfigureTitlePlugin("unexpected-title.so"); return 1; }
  catch (const std::runtime_error&) {}
  const auto entries = Oot3dWholeAotEntryPoints();
  if (!Oot3dWholeAotPluginV2Available() || entries.size() < 18000 ||
      !Oot3dWholeAotPluginObservesMemoryWrites() ||
      !Oot3dWholeAotPluginExitRequestSupported() ||
      Oot3dWholeAotPluginExecutionActive()) return 2;
  std::printf("{\"static_mm3d_abi\":true,\"entries\":%zu,\"scope\":\"link_and_abi_only\"}\n", entries.size());
  if (argc == 2) {
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<uint8_t> code((std::istreambuf_iterator<char>(input)), {});
    if (code.size() != 5967872) return 3;
    NativeA32Memory memory;
    if (!memory.MapRegion({"private-code", 0x100000, code.size(), false, true, code})) return 4;
    oot3d::recomp::a32::GuestState state{};
    oot3d::recomp::a32::ExecutionResult result{};
    Oot3dWholeAotStats stats{};
    uint32_t consumed = 0;
    // Execute the real entry block with a strict budget, before host services.
    // This is an AOT CPU smoke, not a game session or rendering qualification.
    const bool executed = ExecuteOot3dWholeAotFunction(0x100000, state, memory,
        &result, &stats, nullptr, 1, &consumed, nullptr, nullptr, nullptr, 0,
        nullptr, false, 0);
    if (!executed || consumed != 1 || result.kind != oot3d::recomp::a32::ExitKind::BlockLimit ||
        result.pc != 0x100024 || state.r[14] != 0x100004) return 5;
    std::puts("{\"mm3d_entry_block_executed\":true,\"scope\":\"cpu_smoke_not_game_boot\"}");
  }
}
