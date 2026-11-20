#include "../runtime/save_sync_policy.h"
#include <cassert>
int main() {
 using namespace ReMaskedSaveSync;
 assert(Decide("","","")==Action::None);
 assert(Decide("a","","")==Action::Upload);
 assert(Decide("","a","")==Action::Download);
 assert(Decide("a","a","")==Action::None);
 assert(Decide("a","b","")==Action::Conflict);
 assert(Decide("a","b","a")==Action::Download);
 assert(Decide("b","a","a")==Action::Upload);
 assert(Decide("b","c","a")==Action::Conflict);
 assert(Decide("","a","a")==Action::Download);
 assert(Decide("a","","a")==Action::Conflict);
 assert(SaveName("save0.bin") && SaveName("save2.bin") && !SaveName("system.dat"));
 assert(!SaveName("../save0.bin") && !SaveName("save3.bin") && !SaveName("config.json"));
}
