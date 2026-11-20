// SPDX-License-Identifier: GPL-2.0-or-later
#include "oot3d_menu_model.h"
#include "oot3d_menu_prompts.h"
#include "oot3d_menu_presentation.h"
#include "oot3d_menu_font.h"
#include <iostream>
#include <stdexcept>
using namespace Oot3dNativeGame::Menu;
void Check(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}
int main(){try{
 MenuFontTextureProvider provider(nullptr);oot3d::ui::UiTextureIdentity id;id.semantic_name="menu/image/remasked-icon/0";
 oot3d::ui::UiTexturePixels pixels;Check(provider.Resolve(id,pixels,nullptr)&&pixels.provisional,"resource handoff has a provisional image");
 auto revision=provider.Revision();pixels={};pixels.width=2;pixels.height=2;pixels.rgba8.assign(16,255);
 Check(RegisterMenuImage(id.semantic_name,pixels),"register app artwork");Check(provider.Revision()!=revision,"late artwork invalidates provisional cache");
 oot3d::ui::UiTexturePixels resolved;Check(provider.Resolve(id,resolved,nullptr)&&!resolved.provisional&&resolved.rgba8==pixels.rgba8,"exact app artwork resolves");
 for(float width:{240.f,320.f,426.6667f,520.f})for(float density:{1.f,1.72f,2.45f}){
   int selected=0;Document doc;Tab tab;tab.Title="ReMasked";Row row;row.Key="App icon";row.Kind=RowKind::Choice;
   for(int i=0;i<5;i++){row.Choices.push_back("Icon "+std::to_string(i));row.ChoiceImages.push_back("menu/image/remasked-icon/"+std::to_string(i));}
   row.Read=[&]{return selected;};row.Write=[&](int value){selected=value;return WriteResult{};};tab.Rows.push_back(row);doc.Tabs.push_back(tab);Model model(doc);model.Open();model.Update({}, {},.02);
   Input confirm;confirm.Confirm=true;model.Update(confirm,{},.02);model.Update({}, {},.02);
   for(int choice=0;choice<5;choice++){
     if(choice){Input down;down.Down=true;model.Update(down,{},.02);model.Update({}, {},.02);}
     std::vector<oot3d::ui::UiPrimitive> draw;AppendMenuPresentation(model,width,240,draw,{},HintsFor(PromptSet::Touch),{kDp*density,true});
     bool preview=false;for(auto& p:draw)if(p.texture.semantic_name==row.ChoiceImages[choice]){preview=true;Check(p.destination.x>=0&&p.destination.y>=0&&p.destination.x+p.destination.width<=width+.01f&&p.destination.y+p.destination.height<=240.01f,"preview stays within responsive pane");}
     Check(preview,"focused choice artwork remains visible while list scrolls");
     bool target=false;for(auto& hit:model.CurrentLayout().Regions)if(hit.Kind==HitKind::Choice&&hit.Index==size_t(choice)){target=true;}
     Check(target,"preview shares existing clickable choice target");
   }
   model.Update(confirm,{},.02);model.Update({}, {},.02);Check(selected==4,"controller confirms displayed icon");
   model.Update(confirm,{},.02);model.Update({}, {},.02);Input up;up.Up=true;model.Update(up,{},.02);model.Update({}, {},.02);
   std::vector<oot3d::ui::UiPrimitive> draw;AppendMenuPresentation(model,width,240,draw,{},HintsFor(PromptSet::Touch),{kDp*density,true});
   for(auto& hit:model.CurrentLayout().Regions)if(hit.Kind==HitKind::Choice&&hit.Index==3){Pointer pointer;pointer.Present=true;pointer.Touch=true;pointer.X=hit.Area.X+hit.Area.Width/2;pointer.Y=hit.Area.Y+hit.Area.Height/2;pointer.Pressed=true;model.Update({},pointer,.02);pointer.Pressed=false;model.Update({},pointer,.02);break;}
   Check(selected==3,"touching preview row selects that icon");
 }
 for(float width:{320.f,426.6667f,520.f})for(float density:{1.f,1.72f,2.45f}){
   Document doc;doc.CategorySidebar=true;
   for(auto title:{"General","Controls","Touch","Graphics","Audio","HUD","Quality of life","Models","Saves","About"}){Tab page;page.Title=title;Row row;row.Key="Test option";row.Kind=RowKind::Toggle;row.Read=[](){return 0;};row.Write=[](int){return WriteResult{};};page.Rows.push_back(row);doc.Tabs.push_back(page);}
   Model model(doc);model.Open();model.Update({}, {},.02);
   for(size_t i=0;i<doc.Tabs.size();++i){model.SelectTab(i);std::vector<oot3d::ui::UiPrimitive> draw;
    AppendMenuPresentation(model,width,240,draw,{},HintsFor(PromptSet::Touch),{kDp*density,true});
    bool selected=false;for(auto hit:model.CurrentLayout().Regions){auto a=hit.Area;Check(a.X>=0&&a.Y>=0&&a.X+a.Width<=width+.01f&&a.Y+a.Height<=240.01f,"sidebar hit target remains in viewport");selected|=hit.Kind==HitKind::Tab&&hit.Index==i;}
    Check(selected,"selected category visible at each density");
   }
   model.SelectTab(0);std::vector<oot3d::ui::UiPrimitive> draw;AppendMenuPresentation(model,width,240,draw,{},HintsFor(PromptSet::Touch),{kDp*density,true});
   auto a=model.CurrentLayout().CategoriesArea;auto stride=model.CurrentLayout().CategoryStride;
   Pointer pointer;pointer.Present=true;pointer.Touch=true;pointer.X=a.X+a.Width/2;pointer.Y=a.Y+a.Height/2;pointer.Pressed=true;
   model.Update({},pointer,.02);pointer.Y-=stride*2.1f;model.Update({},pointer,.02);pointer.Pressed=false;model.Update({},pointer,.02);Check(model.TabIndex()==2,"dragging sidebar reaches later categories");
   Input next;next.TabNext=true;model.Update(next,{},.02);model.Update({}, {},.02);Check(model.TabIndex()==3,"controller shoulders navigate sidebar");
   Input back;back.Back=true;model.Update(back,{},.02);model.Update({}, {},.02);Check(model.IsOpen()&&model.FocusPane()==Pane::Categories,"Back returns to categories");
   Input down;down.Down=true;model.Update(down,{},.02);model.Update({}, {},.02);Check(model.TabIndex()==4,"D-pad navigates categories");
   Input confirm;confirm.Confirm=true;model.Update(confirm,{},.02);model.Update({}, {},.02);Check(model.FocusPane()==Pane::Rows,"Confirm enters category");
   model.Update(back,{},.02);model.Update({}, {},.02);model.Update(back,{},.02);Check(!model.IsOpen(),"Back from categories resumes game");
 }
 std::cout<<"Inline icon images, late registration, responsive scrolling, controller and touch selection passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
