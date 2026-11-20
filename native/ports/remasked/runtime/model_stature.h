// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <nlohmann/json.hpp>
namespace ReMasked {
inline bool TallPakModel(const nlohmann::json& report) {
    if(!report.is_object()||!report.contains("schema")||report["schema"]!=1||
       !report.contains("kind")||report["kind"]!="pak-import")return false;
    if(auto it=report.find("model_stature");it!=report.end()&&it->is_string()) {
        if(*it=="adult")return true;
        if(*it=="child")return false;
    }
    auto form=report.find("source_form");
    return form!=report.end()&&form->is_number_integer()&&*form==0;
}
}
