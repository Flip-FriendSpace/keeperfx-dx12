// SPDX-License-Identifier: GPL-2.0-or-later
#include "kfx/renderer/PresentationLighting.h"
#include "kfx/renderer/d3d12/LightingRemaster.h"
#include "kfx/renderer/d3d12/RuntimeLightmapUpdates.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace kfx::render::lighting;
using namespace kfx::render::dx12;
FrameTag frame{9,100,7};
RGB Sample(const Source& s,const Receiver& r,uint64_t epoch,float fraction) {
    auto f=frame;f.epoch=epoch;
    return Irradiance(s,r,f,{f,s.incarnation,r.identity,s.slot,fraction});
}
uint64_t checks=0;
void Check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
int main(){try{
    const float nan=std::numeric_limits<float>::quiet_NaN();
    Check(SmoothFalloff(0,4)==1 && SmoothFalloff(4,4)==0 && SmoothFalloff(5,4)==0,"Falloff endpoints");
    Check(SmoothFalloff(nan,4)==0 && SmoothFalloff(1,0)==0 && SmoothFalloff(-1,4)==0,"Invalid falloff");
    float previous=1;for(int n=0;n<=10000;++n){float value=SmoothFalloff(n*0.001f,4);Check(value>=0&&value<=previous,"Monotone bounded falloff");previous=value;}
    Source light{9,17,2,{0,0,1},4,1,{1,0.25f,0.05f},true};
    Receiver terrain{{0,0,0},{0,0,1},0,true,false,false};terrain.identity=41;light.sampled_sequence=100;auto sprite=terrain;
    const RGB zero{},base{0.12f,0.1f,0.08f};
    auto lit=Sample(light,terrain,9,1);Check(lit[0]>lit[1]&&lit[1]>lit[2],"Explicit colour");
    Check(lit==Sample(light,sprite,9,1),"Terrain/sprite contract consistency");
    Check(Sample(light,terrain,8,1)==zero,"Old epoch");
    Check(Sample(light,terrain,9,0)==zero,"Closed door/occlusion");
    Check(Sample(light,terrain,9,nan)==zero,"Unknown occlusion");
    auto hidden=terrain;hidden.revealed=false;Check(Sample(light,hidden,9,1)==zero,"Concealed receiver");
    auto source=light;source.revealed=false;Check(Sample(source,terrain,9,1)==zero,"Concealed source");
    source=light;source.incarnation=0;Check(Sample(source,terrain,9,1)==zero,"Unknown source identity");
    auto owner=terrain;owner.ownership_protected=true;Check(DiffuseResponse(base,owner,lit,0.25f)==base,"Ownership protection");
    auto painted=terrain;painted.baked_fraction=1;Check(DiffuseResponse(base,painted,lit,0.25f)==base,"Baked highlight protection");
    Check(DiffuseResponse(base,terrain,lit,0)==base,"Disabled exact fallback");
    auto bright=DiffuseResponse(base,terrain,lit,0.25f);for(unsigned c=0;c<3;++c)Check(bright[c]>=base[c]&&bright[c]<=base[c]+(1-base[c])*0.25f,"Bounded response");
    auto normal=terrain;normal.normal_authored=true;normal.normal={0,0,-1};Check(Sample(light,normal,9,1)==zero,"Back-facing authored normal");
    normal.normal={0,0,0};Check(Sample(light,normal,9,1)==zero,"Invalid authored normal");
    // Moving source and changed geometry must affect this frame only, even
    // when slot identity is stable. No history filter crosses the boundary.
    for(unsigned n=0;n<64;++n){source=light;source.position.x=float(n)/8;const float visibility=n>=24&&n<40?0.0f:1.0f;
        auto value=Sample(source,terrain,9,visibility);if(visibility==0)Check(value==zero,"Door/construction exclusion");
        Check(value==Sample(source,sprite,9,visibility),"Moving receiver parity");}
    PairVisibility stale{frame,light.incarnation,terrain.identity,light.slot,1};
    auto next=frame;++next.geometry_revision;
    Check(Irradiance(light,terrain,next,stale)==zero,"Stale geometry visibility");
    next=frame;++next.sequence;
    Check(Irradiance(light,terrain,next,stale)==zero,"Stale frame visibility");
    stale.source_incarnation++;
    Check(Irradiance(light,terrain,frame,stale)==zero,"Reused source slot");
    for(uint32_t f=0;f<8;++f)Check(ValidLightingRemaster(f),"Control combination");
    Check(!ValidLightingRemaster(8)&&!ValidLightingRemaster(UINT32_MAX),"Unknown controls");
    // Skipped source/geometry revisions must compare the actual GPU baseline.
    constexpr size_t bytes=511*511*2;std::vector<uint8_t> old(bytes),target(bytes);
    old[(17*511+18)*2]=77;target[(20*511+24)*2]=99;
    auto plan=PlanRuntimeLightmapUpdates(old,target);Check(plan.upload_bytes==4,"Moving light clears old footprint");
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"gpuValidated\":false,\"userAccepted\":false}\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
