// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace MM3D::DialogueText {
inline std::uint64_t Hash(std::string_view text) {
    std::uint64_t hash=0xcbf29ce484222325ULL;
    for(unsigned char byte:text)hash=(hash^byte)*0x100000001b3ULL;
    return hash;
}
struct Recipe {
    std::uint32_t source_size;
    std::uint64_t source_hash;
    std::uint32_t length;
    std::uint64_t hash;
    std::string_view commands;
    constexpr std::size_t size() const {return length;}
};
// Originals exist only in process memory, never in a shipped table or cache file.
// A bound prevents malformed input from retaining unbounded game memory.
struct OriginalStore {
    std::unordered_map<std::uint64_t,std::string> texts;
    std::size_t retained_bytes=0;
};
inline OriginalStore& Originals() {
    static thread_local OriginalStore store;
    return store;
}
inline std::optional<std::string> Resolve(const Recipe& recipe,std::string_view current) {
    if(!recipe.source_size||recipe.source_size>4096||recipe.length>4096)return std::nullopt;
    auto& store=Originals();
    if(current.size()==recipe.source_size&&Hash(current)==recipe.source_hash&&
       !store.texts.contains(recipe.source_hash)) {
        if(store.retained_bytes+current.size()>4*1024*1024)return std::nullopt;
        store.texts.emplace(recipe.source_hash,current);
        store.retained_bytes+=current.size();
    }
    const auto found=store.texts.find(recipe.source_hash);
    if(found==store.texts.end()||found->second.size()!=recipe.source_size)return std::nullopt;
    const auto& source=found->second;
    if(recipe.commands.empty()) {
        if(recipe.length!=source.size()||recipe.hash!=recipe.source_hash)return std::nullopt;
        return source;
    }
    std::string result;
    auto commands=recipe.commands;
    while(!commands.empty()) {
        if(commands.size()<6)return std::nullopt;
        const auto word=[&](unsigned at){return unsigned(static_cast<unsigned char>(commands[at]))|
            (unsigned(static_cast<unsigned char>(commands[at+1]))<<8);};
        const auto offset=word(0),count=word(2),literal=word(4);
        commands.remove_prefix(6);
        if(offset>source.size()||count>source.size()-offset||literal>commands.size()||
           count+literal>recipe.length-result.size())return std::nullopt;
        result.append(source,offset,count);
        result.append(commands.substr(0,literal));
        commands.remove_prefix(literal);
    }
    if(result.size()!=recipe.length||Hash(result)!=recipe.hash)return std::nullopt;
    return result;
}
template<class M> std::optional<std::string> Read(M& memory,std::uint32_t start,std::uint32_t size) {
    if(!size||size>4096||start>0xffffffffu-size)return std::nullopt;
    std::string result(size,'\0');
    for(auto at=start&~3u;at<start+size;at+=4) {
        const auto word=memory.Read(at);if(!word)return std::nullopt;
        for(unsigned byte=0;byte<4;++byte)if(at+byte>=start&&at+byte<start+size)
            result[at+byte-start]=char(*word>>(byte*8));
    }
    return result;
}
}
