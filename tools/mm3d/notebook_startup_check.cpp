#include "mm3d/native_notebook.h"
#include <cassert>
struct Memory {
 unsigned reads{};
 std::optional<std::uint32_t> Read(std::uint32_t at) {assert(at>=0x100000);++reads;return {};}
};
int main(){
 MM3D::NativeNotebook::Hook hook;Memory m;
 for(auto owner:{0u,4u,0x80000u}){hook.notebook_owner=owner;hook.detail_opacity=0.f;hook.MapFade(m);assert(hook.detail_opacity==1.f);assert(m.reads==0);}
 hook.notebook_owner=0x08000000;hook.MapFade(m);assert(m.reads==3);
}
