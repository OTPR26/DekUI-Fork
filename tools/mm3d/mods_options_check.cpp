#include "mm3d/model_mod.h"
#include <array>
#include <cassert>
int main() {
 using namespace MM3D;
 assert(ModelMod::DarkSelected(R"({"dark_link_model":true})"));
 assert(ModelMod::AppearanceSelected(R"({"dark_link_model":true,"adult_link_model":false})"));
 assert(ModelMod::AppearanceSelected(R"({"adult_link_model":true})"));
 assert(!ModelMod::AppearanceSelected(R"({"adult_link_model":false,"dark_link_model":false})"));
 for(auto bad:{"", "null", "[]", "{}", R"({"dark_link_model":1})",R"({"dark_link_model":"true"})"})assert(!ModelMod::DarkSelected(bad));
 assert(ModelMod::AdultSelected(R"({"adult_link_model":true})"));
 assert(ModelMod::MasterSwordSelected(R"({"adult_link_master_sword":true})"));
 for(auto bad:{"", "null", "[]", "{}", R"({"adult_link_master_sword":1})",R"({"adult_link_master_sword":"true"})"})assert(!ModelMod::MasterSwordSelected(bad));
 assert(ModelMod::SelectedTunic(R"({"adult_link_tunic_color":2})")==2);
 for(auto bad:{"{}",R"({"adult_link_tunic_color":-1})",R"({"adult_link_tunic_color":7})",R"({"adult_link_tunic_color":"red"})"})assert(ModelMod::SelectedTunic(bad)==0);
 const std::array<std::uint8_t,12> original{20,150,25,217,150,100,40,255,190,160,130,255};
 for(int color=0;color<=6;++color) {
  auto pixels=original;TunicColor::selected=color;TunicColor::Apply(0xF855116F497CFEC2ULL,pixels);
  assert(pixels[3]==217);for(int i=4;i<12;++i)assert(pixels[i]==original[i]);
  if(color<=2)assert(pixels==original);else assert(pixels[0]!=original[0]||pixels[1]!=original[1]||pixels[2]!=original[2]);
  auto unrelated=original;TunicColor::Apply(123,unrelated);assert(unrelated==original);
 }
 // Original blue/red are verbatim textures extracted from OoT3D's clothing CMAB.
 TunicColor::selected=0;
}
