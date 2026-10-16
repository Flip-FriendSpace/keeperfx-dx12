// SPDX-License-Identifier: GPL-2.0-or-later
#include "kfx/renderer/EmissivePresentation.h"
#include "kfx/renderer/TerrainTextureResolution.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <string_view>
#include <vector>
using namespace kfx::render;
int main(int argc,char** argv) {
    if(argc==2) {
        std::cout<<(EmissivePresentationEnabled()?"enabled":"disabled")<<'\n';
        return EmissivePresentationEnabled()!=(std::string_view(argv[1])=="enabled");
    }
    unsigned checks=0;
    // Independent integer reference to all 256 encoded indices, including
    // inactive slots, out-of-range ordinary palette indices and dark family6.
    constexpr int rgb[8][3]={{16,16,16},{24,16,0},{24,4,4},{8,8,24},
        {8,24,8},{12,0,12},{8,8,8},{24,12,4}};
    for(unsigned i=0;i<256;++i) {
        const auto actual=PositiveEncodedEmission(uint8_t(i));
        for(unsigned k=0;k<3;++k) {
            const bool valid=i>=1&&i<=64&&(i-1)/8!=6;
            const float expected=valid?float(rgb[(i-1)/8][k]*((i-1)%8))/255:0;
            assert(std::abs(actual[k]-expected)<1e-6f);++checks;
        }
    }
    assert(ClassifyEmissiveSource(110,false,3,89)==EmissiveSource::None);
    assert(ClassifyEmissiveSource(110,false,3,37)==EmissiveSource::Portal);
    assert(ClassifyEmissiveSource(791,false)==EmissiveSource::None); // candle body
    assert(ClassifyEmissiveSource(962,false)==EmissiveSource::None); // torch bracket
    assert(ClassifyEmissiveSource(948,false,1,5)==EmissiveSource::Heart);
    assert(ClassifyEmissiveSource(927,false,2,1)==EmissiveSource::Spell);
    assert(ClassifyEmissiveSource(0,true)==EmissiveSource::EncodedGlow);
    assert((EmissiveParameters(EmissiveSource::None)==std::array<float,4>{}));
    checks+=8;
    // Exact accepted Heart alias from runtime receipt, plus independent native
    // and symbolic spell/portal aliases. Shared artwork is not consumer identity.
    for(uint32_t alias:{948u,949u,19804u,19860u,32768u}) {
        assert(ClassifyEmissiveSource(alias,false,1,5)==EmissiveSource::Heart);
        assert(ClassifyEmissiveSource(alias,false,1,3)==EmissiveSource::None);
        assert(ClassifyEmissiveSource(alias,false,3,37)==EmissiveSource::Portal);
        assert(ClassifyEmissiveSource(alias,false,3,89)==EmissiveSource::None);
        for(uint32_t shot:{1u,3u,5u,10u,24u,25u,28u})
            assert(ClassifyEmissiveSource(alias,false,2,shot)==EmissiveSource::Spell);
        checks+=11;
    }
    // These exact object consumers share flame artwork with unrelated effects.
    // Both native and custom aliases must retain native glow, including negatives.
    for(uint32_t alias:{798u,799u,23466u}) {
        for(uint32_t model:{111u,120u,121u,122u,162u,165u,167u,169u}) {
            assert(ClassifyEmissiveSource(alias,true,1,model)==EmissiveSource::None);
            assert(ClassifyEmissiveSource(alias,true,3,model)==EmissiveSource::EncodedGlow);
            checks+=2;
        }
        assert(ClassifyEmissiveSource(alias,true,1,2)==EmissiveSource::EncodedGlow);
        assert(ClassifyEmissiveSource(alias,true,1,28)==EmissiveSource::EncodedGlow);
        assert(ClassifyEmissiveSource(alias,true,3,1)==EmissiveSource::EncodedGlow);
        checks+=3;
    }
    assert((EmissiveParameters(EmissiveSource::Heart)==std::array<float,4>{}));++checks;
    // Real pointer provenance boundaries, including adjacent ghost row0 and
    // interior pointers, must never masquerade as authenticated shade rows.
    std::array<uint8_t,65*256> tables{};
    for(size_t i=0;i<tables.size();++i)tables[i]=uint8_t(i);
    const auto saved=tables;
    const std::span<const uint8_t> fade(tables.data(),64*256);
    const auto resolve=[&](const uint8_t* remap,bool enabled=true,bool cursor=false,
        bool additive=false,uint32_t cls=1,uint32_t model=5) {
        return HeartPresentationRemap(enabled,cursor,additive,cls,model,remap,fade);
    };
    for(unsigned row=0;row<64;++row) {
        const auto* native=tables.data()+row*256;
        const auto* result=resolve(native);
        if(row<=32)assert(result==native);
        else assert(result>=tables.data()+32*256 && result<=tables.data()+34*256);
        assert(resolve(native,false)==native && resolve(native,true,true)==native);
        assert(resolve(native,true,false,true)==native);
        assert(resolve(native,true,false,false,3,5)==native);
        assert(resolve(native,true,false,false,1,111)==native);checks+=5;
        if(row>32)assert(result>=resolve(native-256));
        assert(resolve(native+1)==native+1);++checks;
    }
    assert(resolve(tables.data()+33*256)==tables.data()+32*256);
    assert(resolve(tables.data()+47*256)==tables.data()+32*256);
    assert(resolve(tables.data()+48*256)==tables.data()+33*256);
    assert(resolve(tables.data()+62*256)==tables.data()+33*256);
    assert(resolve(tables.data()+63*256)==tables.data()+34*256);
    assert(resolve(tables.data()+64*256)==tables.data()+64*256); // ghost row0
    std::array<uint8_t,256> flash{};
    assert(resolve(flash.data())==flash.data() && resolve(nullptr)==nullptr);
    assert(HeartPresentationRemap(true,false,false,1,5,tables.data()+63*256,
        std::span<const uint8_t>(tables.data(),256))==tables.data()+63*256);
    assert(tables==saved);checks+=9;
    for(unsigned t=0;t<1544;++t)for(unsigned v=0;v<32;++v) {
        assert(EmissiveLavaMaterial(t,v)==(v==0&&((t>=56&&t<=71)||t==546||t==547)));++checks;
    }
    // A non-empty frame, a blank transition and a reused slot must never retain
    // energy from another phase/incarnation. Input is immutable and raw-index
    // glow deliberately ignores coverage, exactly like the production shader.
    std::vector<uint8_t> frame(8*8*4,0);
    for(size_t i=0;i<64;++i)frame[i*4]=16; // orange, strength7
    const auto original=frame;
    EmissiveFrameEnergy id;id.epoch=9;id.slot=3;id.incarnation=41;id.frame=7;id.source_revision=2;
    const auto a=SampleEncodedFrame(frame,8,8,32,id);
    assert(a.valid&&a.epoch==9&&a.slot==3&&a.incarnation==41&&a.frame==7&&a.source_revision==2);
    assert(std::abs(a.gain-168.f/255)<1e-6f&&frame==original);
    std::fill(frame.begin(),frame.end(),0);id.frame=8;id.incarnation=42;
    const auto b=SampleEncodedFrame(frame,8,8,32,id);
    assert(b.valid&&b.gain==0&&b.frame==8&&b.incarnation==42);
    for(size_t i=0;i<64;++i)frame[i*4]=56; // negative family
    assert(SampleEncodedFrame(frame,8,8,32,id).gain==0);
    assert(!SampleEncodedFrame(frame,8,8,31,id).valid);
    assert(!SampleEncodedFrame(frame,9,8,36,id).valid);
    assert(!SampleEncodedFrame(frame,0,8,32,id).valid);
    assert(!SampleEncodedFrame(frame,1025,8,4100,id).valid);checks+=8;
    // Material bit cannot alter atlas origin or the HD resolution encoding.
    for(unsigned d:{32u,64u,128u,256u}) {
        const auto ref=TerrainTextureReference(0,0,d);
        assert(((ref|0x10000000u)&0x0fffffffu)==ref);++checks;
    }
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"scope\":\"CPU source metadata and immutable frame energy\"}\n";
}
