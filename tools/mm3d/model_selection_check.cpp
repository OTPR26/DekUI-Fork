#include "mm3d/model_mod.h"
#include <cassert>
int main() {
 using MM3D::ModelMod::AdultSelected;
 assert(AdultSelected(R"({"adult_link_model":true})"));
 for(auto text:{"", "null", "[]", "{", "{}", R"({"adult_link_model":false})", R"({"adult_link_model":1})", R"({"adult_link_model":"true"})"})assert(!AdultSelected(text));
 assert(AdultSelected(R"({"adult_link_model":true,"camera_distance":1.65,"hud_diamond":{}})"));
 assert(MM3D::ModelMod::PakSelected(R"({"pak_link_model":true})"));
 for(auto text:{"", "null", "[]", "{}", R"({"pak_link_model":false})", R"({"pak_link_model":1})", R"({"pak_link_model":"true"})"})assert(!MM3D::ModelMod::PakSelected(text));
 // PAK models use the child-height rig, never the tall-model correction.
 assert(!MM3D::ModelMod::AppearanceSelected(R"({"pak_link_model":true})"));
}
