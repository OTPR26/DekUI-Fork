#include "../runtime/model_stature.h"
#include <cassert>
int main(){
 using nlohmann::json;json report={{"schema",1},{"kind","pak-import"}};
 assert(!ReMasked::TallPakModel(report));
 for(int form:{0,1,4,104}){report["source_form"]=form;assert(ReMasked::TallPakModel(report)==(form==0));}
 report["model_stature"]="adult";report["source_form"]=4;assert(ReMasked::TallPakModel(report));
 report["model_stature"]="child";report["source_form"]=0;assert(!ReMasked::TallPakModel(report));
 report.erase("model_stature");report["source_form"]="0";assert(!ReMasked::TallPakModel(report));
 report["source_form"]=0;report["schema"]=2;assert(!ReMasked::TallPakModel(report));
 assert(!ReMasked::TallPakModel(json::array()));assert(!ReMasked::TallPakModel(nullptr));
}
