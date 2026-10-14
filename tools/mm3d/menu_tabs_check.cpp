#define main inventory_legacy_main
#include "native_inventory_check.cpp"
#undef main
#include "mm3d/menu_tabs.h"
int main(){
 Memory m;MM3D::MenuTabs::Hook hook;Hook inventory;
 auto groups=m.Alloc(8),entries=m.Alloc(84);
 m.words[MM3D::NotebookPrompt::RegistryLiteral]=MM3D::NotebookPrompt::Registry;
 m.words[MM3D::NotebookPrompt::Registry+0x28]=groups;m.words[MM3D::NotebookPrompt::Registry+0x2c]=entries;m.words[groups]=21;
 for(U32 id:{15,20}){auto record=m.Alloc(0x60);m.words[record+4]=id<<16;m.words[entries+id*4]=record;}
 const auto stack=m.Alloc(0x9008),sp=(stack+0x9008)&~7u;
 for(auto name:{"MenuItem","MenuMask","MenuCollect"}){
  auto layout=m.Layout(name),all=m.Widget("all"),tabs=m.Widget("btn_item_omen_g"),pane=m.Widget("pane"),geo=m.Alloc(0x7c);
  m.Children(layout+8,{all});m.Children(all,{tabs,pane});m.words[pane+0x28]=geo;m.words[geo+4]=6;
  m.words[all+0x134]=std::bit_cast<U32>(.5f);m.words[tabs+0x134]=std::bit_cast<U32>(.5f);
  inventory.panel=layout;inventory.pass=3;inventory.routed={layout};
  auto before=m.words;auto root=hook.Root(m,inventory,layout,sp,layout+8);
  const unsigned selected=std::string(name)=="MenuCollect"?0:std::string(name)=="MenuMask"?1:2;
  const unsigned count=selected?5:4;
  assert(root==sp-0x2000);assert(m.words[root+0x18]==count+1);
  auto list=m.words[root+0x14];assert(m.words[list+count*4]==layout+8);
  assert(!(m.words[tabs+0xd4]&1));
  for(unsigned i=0;i<count;++i){auto n=m.words[list+i*4];assert(std::bit_cast<float>(m.words[n+0x134])==.5f);}
  for(unsigned i=0;i<count;++i){
   const bool pressed=i>=selected&&i<selected+(selected?3:2);
   auto n=m.words[list+i*4],g=m.words[n+0x28];
   const unsigned tab=i<selected?i:pressed?selected:i-(selected?2:1);
   assert(m.words[g+0x10]==hook.Texture(m,tab?20:15));
   const auto& art=MM3D::MenuTabs::Panes[tab*4+(pressed?i-selected+1:0)];
   for(unsigned uv=0;uv<4;++uv)assert(m.words[g+0x28+uv*4]==art[5+uv]);
   assert(std::bit_cast<float>(m.words[g+0x18])==(pressed?-7.f:0.f));
   for(unsigned c=0;c<3;++c)
    assert(std::bit_cast<float>(m.words[n+0x128+c*4])==(pressed?1.f:.7f));
   for(unsigned c=0;c<16;++c)
    assert(std::bit_cast<float>(m.words[g+0x3c+c*4])==1.f);
  }
  // Native pause atlas face coordinates: Gear leaves both tabs unselected.
  for(unsigned tab=1;tab<3;++tab){
   const unsigned face=tab+(selected<tab?(selected?2:1):0);
   const auto g=m.words[m.words[list+face*4]+0x28];
   const float u=std::bit_cast<float>(m.words[g+0x28]);
   const float v=std::bit_cast<float>(m.words[g+0x2c]);
   assert(u==(selected==tab?.5f:.484375f));
   assert(v==(tab==1?(selected==tab?.390625f:.125f):(selected==tab?.25f:0.f)));
   assert(std::abs(std::bit_cast<float>(m.words[g+0x20])-82.f)<.001f);
   if(selected==tab){
    auto left=m.words[m.words[list+(face+1)*4]+0x28];
    auto right=m.words[m.words[list+(face+2)*4]+0x28];
    assert(std::bit_cast<float>(m.words[left+0x20])==41.f);
    assert(std::bit_cast<float>(m.words[right+0x14])==41.f);
    assert(std::bit_cast<float>(m.words[right+0x30])<0); // mirrored second half
   }
  }
  assert(hook.scratch.End(m)==sp&&m.words==before);
  // Opening and closing use the same native progress in opposite order.
  for(float remaining:{0.f,.25f,.75f,1.f,.75f,.25f,0.f}){
   inventory.slide_active=std::string(name)!="MenuCollect";
   inventory.slide_remaining=remaining;
   inventory.gear_slide_active=std::string(name)=="MenuCollect";
   inventory.gear_left_x=-264*remaining;
   root=hook.Root(m,inventory,layout,sp,layout+8);list=m.words[root+0x14];
   for(unsigned i=0;i<count;++i){auto n=m.words[list+i*4];
    assert(std::abs(std::bit_cast<float>(m.words[n+0xfc])-(204+40*remaining))<.001f);
   }
   hook.scratch.End(m);assert(m.words==before);
  }
  // Mid-transition tabs remain visibly inside the viewport, including when
  // moved up by the editor; all layers use the same clearance.
  MM3D::MenuTabs::gear_pose.y=-100;
  inventory.slide_active=true;inventory.slide_remaining=.5f;
  inventory.gear_slide_active=false;
  root=hook.Root(m,inventory,layout,sp,layout+8);list=m.words[root+0x14];
  assert(std::abs(std::bit_cast<float>(m.words[m.words[list]+0xfc])-173.f)<.001f);
  hook.scratch.End(m);assert(m.words==before);MM3D::MenuTabs::gear_pose.y=0;
  inventory.slide_active=inventory.gear_slide_active=false;inventory.slide_remaining=0;
  inventory.pass=5;assert(hook.Root(m,inventory,layout,sp,layout+8)==layout+8);hook.scratch.End(m);assert(m.words==before);
 }
}
