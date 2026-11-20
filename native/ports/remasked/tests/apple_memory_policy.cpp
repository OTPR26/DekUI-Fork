#include "../runtime/apple_memory_policy.h"
#include <cassert>
#include <iostream>
int main() {
    ReMasked::AppleMemoryPolicy p;
    assert(p.DecodedMiB(1000, 500, 2800, true) == 96);
    assert(p.DecodedMiB(1000, 500, 2800, false) == 192);
    p.Warn(1000, true);
    assert(p.DecodedMiB(2000, 500, 2800, true) == 16);
    p.Warn(2000, false); // A less severe event cannot undo the critical hold.
    assert(p.DecodedMiB(3000, 500, 2800, true) == 16);
    assert(p.DecodedMiB(31001, 500, 2800, true) == 32);
    assert(p.DecodedMiB(32001, 500, 2800, true) == 96);
    assert(p.DecodedMiB(32001, 1600, 1700, true) == 32);
    assert(p.DecodedMiB(32001, 500, 200, true) == 16);
    p.Warn(40000, true); p.Warn(35000, true);
    assert(p.DecodedMiB(69999, 500, 2800, true) == 16);
    assert(p.DecodedMiB(70000, 500, 2800, true) == 96);
    std::cout << "Apple memory pressure recovery passed\n";
}
