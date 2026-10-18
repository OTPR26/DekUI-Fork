// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <string_view>
#include <json.hpp>
#include "mm3d/tunic_color.h"

namespace MM3D::ModelMod {
// Selection is sampled when RomFS opens, never during a running game's asset reads.
// Legacy flag name: true for either imported tall human model.
inline std::atomic<bool> adult_active{false};
inline constexpr std::string_view actor_path="actors/zelda2_link_child_new.gar.lzs";
inline bool DarkSelected(std::string_view text) {
    const auto object=nlohmann::json::parse(text,nullptr,false);
    if(!object.is_object())return false;
    const auto it=object.find("dark_link_model");
    return it!=object.end() && it->is_boolean() && it->get<bool>();
}
inline bool MasterSwordSelected(std::string_view text) {
    const auto object=nlohmann::json::parse(text,nullptr,false);
    if(!object.is_object())return false;
    const auto it=object.find("adult_link_master_sword");
    return it!=object.end() && it->is_boolean() && it->get<bool>();
}
inline int SelectedTunic(std::string_view text) {
    const auto object=nlohmann::json::parse(text,nullptr,false);
    if(!object.is_object())return 0;
    const auto it=object.find("adult_link_tunic_color");
    if(it==object.end()||!it->is_number_integer())return 0;
    const auto value=it->get<std::int64_t>();return value>=0&&value<=6?int(value):0;
}
inline bool AdultSelected(std::string_view text) {
    const auto object=nlohmann::json::parse(text,nullptr,false);
    if(!object.is_object())return false;
    const auto it=object.find("adult_link_model");
    return it!=object.end() && it->is_boolean() && it->get<bool>();
}
inline bool AppearanceSelected(std::string_view text) {
    return DarkSelected(text) || AdultSelected(text);
}
}
