// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "Dx12Device.h"
#include <fstream>
#include <stdexcept>

namespace kfx::render::dx12::enhanced_test {
inline void Require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
inline void Save(const std::filesystem::path& stem, const Image& image)
{
    std::ofstream raw(stem.string()+".rgba",std::ios::binary);
    raw.write(reinterpret_cast<const char*>(image.rgba.data()),std::streamsize(image.rgba.size()));
    Require(bool(raw),"Cannot retain enhanced sprite RGBA evidence");
    std::ofstream file(stem.string()+".bmp",std::ios::binary);
    auto word=[&](uint16_t n){file.put(char(n));file.put(char(n>>8));};
    auto dword=[&](uint32_t n){word(uint16_t(n));word(uint16_t(n>>16));};
    file.put('B');file.put('M');dword(uint32_t(54+image.rgba.size()));
    word(0);word(0);dword(54);dword(40);dword(image.width);dword(image.height);
    word(1);word(32);dword(0);dword(uint32_t(image.rgba.size()));dword(2835);dword(2835);dword(0);dword(0);
    for(uint32_t y=image.height;y-->0;)for(uint32_t x=0;x<image.width;++x){
        const auto p=(size_t(y)*image.width+x)*4;
        file.put(char(image.rgba[p+2]));file.put(char(image.rgba[p+1]));
        file.put(char(image.rgba[p]));file.put(char(image.rgba[p+3]));
    }
    Require(bool(file),"Cannot retain enhanced sprite BMP evidence");
}
inline uint64_t Differences(const Image& a, const Image& b)
{
    Require(a.width==b.width && a.height==b.height && a.rgba.size()==b.rgba.size(),"Enhanced image dimensions differ");
    uint64_t differences=0;
    for(size_t i=0;i<a.rgba.size();++i)differences+=a.rgba[i]!=b.rgba[i];
    return differences;
}
inline std::string Json(const std::string& text)
{
    std::string result="\"";
    for(unsigned char c:text){if(c=='"'||c=='\\')result+='\\';result+=c<32?'?':char(c);}
    return result+'"';
}
}
