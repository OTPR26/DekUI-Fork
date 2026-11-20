// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../../titles/mm3d/adapter/mm3d_restoration.h"
#include "../../../titles/mm3d/restoration/engine_impl.h"
#include "../../../titles/mm3d/restoration/port_tests.inc"
int main() {
    try {
        RestorationPortTests::TatlDpadState();
        std::cout << "Tatl prompt and D-pad ocarina: all five HUD states passed\n";
    } catch(const std::exception& e) {
        std::cerr << e.what() << '\n';return 1;
    }
}
