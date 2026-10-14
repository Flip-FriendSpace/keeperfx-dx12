// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include <algorithm>
#include <cstdint>
#include <span>

namespace kfx::render {
struct HugeSpriteSourceView {
    std::span<const uint8_t> data,line_offsets;
    uint32_t width=0,height=0;
};
namespace huge_detail {
inline uint32_t Read32(const uint8_t* p) noexcept {
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
inline bool Validate(HugeSpriteSourceView source) noexcept {
    if(!source.width || !source.height || source.width>16384 || source.height>16384
        || source.line_offsets.size()<size_t(source.height)*4 || source.data.empty())return false;
    for(uint32_t y=0;y<source.height;++y){
        size_t at=Read32(source.line_offsets.data()+size_t(y)*4);uint32_t x=0;
        while(x<source.width){
            if(at>source.data.size() || source.data.size()-at<4)return false;
            const auto solid=Read32(source.data.data()+at);at+=4;
            if(solid>source.width-x || solid>source.data.size()-at)return false;
            x+=solid;at+=solid;
            if(source.data.size()-at<4)return false;
            const auto skip=Read32(source.data.data()+at);at+=4;
            if(skip>source.width-x || (!solid && !skip))return false;
            x+=skip;
        }
    }
    return true;
}
struct Axis {
    int64_t factor=0,initial=0,first=0;
    uint32_t extent=0;
    bool clipped=false;
    static Axis Make(uint32_t source,uint32_t extent,int32_t shift,int32_t units) noexcept {
        const int64_t destination=int64_t(source)*units/16;
        const int64_t origin=-int64_t(shift)*units/16;
        Axis a;a.extent=extent;a.factor=(destination*65536)/source;a.initial=(a.factor>>1)+origin*65536;
        a.clipped=origin<0 || destination+source+origin>=extent;
        a.first=a.initial>>16;
        if(a.clipped)a.first=std::min(a.first,std::max<int64_t>(0,origin));
        return a;
    }
    int32_t Start(uint32_t x) const noexcept {
        const int64_t value=x?(initial+int64_t(x)*factor)>>16:first;
        return int32_t(clipped?std::clamp<int64_t>(value,0,extent):value);
    }
    int32_t Length(uint32_t x) const noexcept {return std::max(0,Start(x+1)-Start(x));}
};
}
// Validate every encoded row before touching output. Rows contain alternating
// little-endian uint32 literal lengths, literal bytes, and transparent lengths.
// The bounded raster preserves the legacy huge-sprite scaling step arithmetic,
// packed visible rows and opaque palette index zero; it never calls software
// framebuffer routines or touches the global scaling arrays.
inline bool DecodeHugeSprite(HugeSpriteSourceView source,std::span<uint8_t> pixels,std::span<uint8_t> coverage,
    uint32_t width,uint32_t height,int32_t x_shift,int32_t y_shift,int32_t units) noexcept
{
    if(!width || !height || width>16384 || height>16384 || units<=0 || units>65536
        || pixels.size()!=size_t(width)*height || coverage.size()!=pixels.size() || !huge_detail::Validate(source))return false;
    std::fill(pixels.begin(),pixels.end(),0);std::fill(coverage.begin(),coverage.end(),0);
    // The legacy public raster parameters are short; retain that conversion.
    const auto xs=huge_detail::Axis::Make(source.width,width,int16_t(x_shift),units);
    const auto ys=huge_detail::Axis::Make(source.height,height,int16_t(y_shift),units);
    uint32_t out_y=0;
    for(uint32_t y=0;y<source.height;++y){
        const auto rows=std::min<int32_t>(ys.Length(y),int32_t(height)-ys.Start(y));
        if(rows<=0)continue;
        size_t at=huge_detail::Read32(source.line_offsets.data()+size_t(y)*4);uint32_t x=0;int64_t out_x=0;
        while(x<source.width && out_x<width){
            const auto solid=huge_detail::Read32(source.data.data()+at);at+=4;
            for(uint32_t i=0;i<solid;++i,++x){
                const auto count=std::max(0,std::min<int32_t>(xs.Length(x),int32_t(width)-xs.Start(x)));
                for(int32_t n=0;n<count;++n,++out_x)if(out_x>=0 && out_x<width){
                    for(int32_t row=0;row<rows && out_y+uint32_t(row)<height;++row){
                        const auto dst=size_t(out_y+row)*width+size_t(out_x);pixels[dst]=source.data[at+i];coverage[dst]=255;
                    }
                }
            }
            at+=solid;const auto skip=huge_detail::Read32(source.data.data()+at);at+=4;
            const auto previous=x;x+=skip;
            if(x>=source.width)break;
            out_x+=int64_t(xs.Start(x))-xs.Start(previous);
        }
        out_y+=uint32_t(rows);
        if(out_y>=height)break;
    }
    return true;
}
}
