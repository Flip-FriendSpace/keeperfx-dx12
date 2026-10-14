// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Standalone CPU contract tests. Link the unchanged LuaLensEffect.cpp and
// lua_api_lens.c with the game's Lua library. No renderer or simulation runs.
#include "kfx/lense/LuaLensEffect.h"
#include "lua_api_lens.h"
#include "lens_api.h"
#include "globals.h"
extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}
#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::array<unsigned char,768> palette;
unsigned expected_errors=0;
[[noreturn]] void Unused(){throw std::runtime_error("Unexpected manager dependency in isolated callback test");}
void Require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
}
// Only dependency isolation: callbacks and buffer algorithms are the original
// source. The base has no raster behavior; asset/manager APIs fail if invoked.
LensEffect::LensEffect(LensEffectType type,const char* name)
    :m_type(type),m_name(name),m_enabled(true),m_user_data(nullptr){}
LensEffect::~LensEffect()=default;
extern "C" {
GameTurn get_gameturn(){return 0;}
unsigned long text_line_number=0;
int LbErrorLog(const char*,...){++expected_errors;return 0;}
int LbWarnLog(const char*,...){return 0;}
int LbSyncLog(const char*,...){return 0;}
int LbScriptLog(int32_t,const char*,...){return 0;}
const unsigned char* RendererGetActivePalette(){return palette.data();}
void* LensManager_GetInstance(){Unused();}
void* LensManager_GetCustomLens(void*,const char*){Unused();}
TbBool LensManager_RegisterCustomLens(void*,const char*,void*){Unused();}
TbBool LensManager_IsReady(void*){Unused();}
TbBool LensManager_SetLens(void*,long){Unused();}
TbBool LensManager_SetLensByName(void*,const char*){Unused();}
long LensManager_GetActiveLens(void*){Unused();}
const char* LensManager_GetActiveCustomLensName(void*){Unused();}
void* LuaLensEffect_Create(const char*,void*){Unused();}
void LuaLensEffect_SetDrawCallback(void*,int){Unused();}
}

namespace {
constexpr int Width=7,Height=5,SourcePitch=13,DestinationPitch=15,SourceX=3,DestinationX=4,DestinationY=2;
struct Buffers {
    std::vector<unsigned char> source=std::vector<unsigned char>(SourcePitch*(Height+2),173);
    std::vector<unsigned char> destination=std::vector<unsigned char>(DestinationPitch*(Height+4),211);
    Buffers(){for(int y=0;y<Height;++y)for(int x=0;x<Width;++x){S(x,y)=static_cast<unsigned char>(10+y*8+x);D(x,y)=static_cast<unsigned char>(70+y*8+x);}}
    unsigned char& S(int x,int y){return source[size_t(y)*SourcePitch+SourceX+x];}
    unsigned char& D(int x,int y){return destination[size_t(y+DestinationY)*DestinationPitch+DestinationX+x];}
    void Copy(){for(int y=0;y<Height;++y)for(int x=0;x<Width;++x)D(x,y)=S(x,y);}
};
struct Case {const char* name;const char* body;bool modified;bool error;std::function<void(Buffers&)> expected;};
void Run(const Case& item){
    Buffers actual,expected;item.expected(expected);
    lua_State* state=luaL_newstate();Require(state!=nullptr,"Create Lua state");
    luaL_openlibs(state);Lens_register(state);
    const unsigned before_errors=expected_errors;
    try {
        {
            LuaLensEffect effect("cpu_contract",state);
            LuaLensConfig config{};effect.SetConfig(config);Require(effect.Setup(-1),"Set up real Lua effect");
            const std::string script="calls=0; return function(ctx) calls=calls+1; assert(ctx.width==7 and ctx.height==5 and ctx.viewport_x==3); "+std::string(item.body)+" end";
            Require(luaL_loadstring(state,script.c_str())==0,"Compile fixture Lua");
            Require(lua_pcall(state,0,1,0)==0,"Create fixture callback");
            effect.SetDrawCallback(luaL_ref(state,LUA_REGISTRYINDEX));
            LensRenderContext ctx{};ctx.srcbuf=actual.source.data();
            ctx.dstbuf=actual.destination.data()+DestinationY*DestinationPitch+DestinationX;
            ctx.srcpitch=SourcePitch;ctx.dstpitch=DestinationPitch;ctx.width=Width;ctx.height=Height;ctx.viewport_x=SourceX;
            Require(bool(effect.Draw(&ctx))==item.modified,"Unexpected callback return");
            Require(bool(ctx.buffer_copied)==item.modified,"Unexpected callback copied flag");
            lua_getglobal(state,"calls");Require(lua_tointeger(state,-1)==1,"Callback did not run exactly once");lua_pop(state,1);
            Require(actual.source==expected.source,"Source bytes or guards differ");
            Require(actual.destination==expected.destination,"Destination bytes, untouched area or guards differ");
            Require(expected_errors-before_errors==unsigned(item.error),"Unexpected callback errors");
        }
        Lens_cleanup_lua();lua_close(state);
    }catch(...){Lens_cleanup_lua();lua_close(state);throw;}
    std::printf("PASS %s\n",item.name);
}
}
int main(){
    try {
        // Four copies of a six-bit gray palette distinguish nearest-color
        // lowest-index ties from copying the requested duplicate index.
        for(unsigned i=0;i<256;++i)for(unsigned c=0;c<3;++c)palette[i*3+c]=i%64;
        const std::vector<Case> cases{
            {"copy-and-marker","CopyBuffer(ctx.srcbuf,ctx.dstbuf); SubmitPixelBatch(ctx.dstbuf,{{x=2,y=1,w=3,h=2,color=255}}); return true",true,false,
                [](Buffers& b){b.Copy();for(int y=1;y<3;++y)for(int x=2;x<5;++x)b.D(x,y)=255;}},
            {"source-read-branch","assert(GetPixel(ctx.srcbuf,2,1)==20); if GetPixel(ctx.srcbuf,2,1)==20 then SetPixel(ctx.dstbuf,6,4,91) else error('wrong source') end; return true",true,false,
                [](Buffers& b){b.D(6,4)=91;}},
            {"source-mutation-before-copy","SetPixel(ctx.srcbuf,1,2,248); CopyBuffer(ctx.srcbuf,ctx.dstbuf); return true",true,false,
                [](Buffers& b){b.S(1,2)=248;b.Copy();}},
            {"ordered-overlap","SubmitPixelBatch(ctx.dstbuf,{{x=1,y=1,w=3,h=2,color=10},{x=2,y=1,w=3,h=2,color=20}}); ApplyColorOffsetBatch(ctx.dstbuf,{{x=2,y=2,w=1,h=1,offset=-25}}); return true",true,false,
                [](Buffers& b){for(int y=1;y<3;++y){for(int x=1;x<4;++x)b.D(x,y)=10;for(int x=2;x<5;++x)b.D(x,y)=20;}b.D(2,2)=0;}},
            {"partial-success-retains-destination","SetPixel(ctx.dstbuf,0,0,99); return true",true,false,
                [](Buffers& b){b.D(0,0)=99;}},
            {"false-retains-source-mutation","SetPixel(ctx.srcbuf,0,0,123); SetPixel(ctx.dstbuf,1,0,124); return false",false,false,
                [](Buffers& b){b.S(0,0)=123;b.D(1,0)=124;}},
            {"error-retains-source-mutation","SetPixel(ctx.srcbuf,0,0,125); SetPixel(ctx.dstbuf,1,0,126); error('intentional callback failure')",false,true,
                [](Buffers& b){b.S(0,0)=125;b.D(1,0)=126;}},
            {"bounds-and-colour-conversions","assert(GetPixel(ctx.srcbuf,-1,0)==0 and GetPixel(ctx.srcbuf,7,4)==0); SetPixel(ctx.dstbuf,-1,0,17); SetPixel(ctx.dstbuf,0,0,258); SubmitPixelBatch(ctx.dstbuf,{{x=6,y=4,w=3,h=3,color=999},{x=-1,y=1,w=2,h=1,color=-5}}); CopyPixel(ctx.srcbuf,ctx.dstbuf,3,2); return true",true,false,
                [](Buffers& b){b.D(0,0)=2;b.D(6,4)=255;b.D(0,1)=0;b.D(3,2)=b.S(3,2);}},
            {"remap-owned-table-and-overlap","SetPixel(ctx.dstbuf,0,0,10); RemapPixelBatch(ctx.dstbuf,{[11]=512,[1]=9},{{x=0,y=0,w=1,h=1},{x=0,y=0,w=1,h=1}}); return true",true,false,
                [](Buffers& b){b.D(0,0)=9;}},
            {"rgb-blend-rounding-and-palette-ties","SetPixel(ctx.dstbuf,0,0,10); SetPixel(ctx.dstbuf,1,0,10); BlendColorBatch(ctx.dstbuf,{{x=0,y=0,w=1,h=1,color=114,alpha=128},{x=1,y=0,w=1,h=1,color=114,alpha=255},{x=2,y=0,w=1,h=1,color=0,alpha=0}}); return true",true,false,
                [](Buffers& b){b.D(0,0)=30;b.D(1,0)=50;}},
            {"darkening-lut","SetPixel(ctx.dstbuf,0,0,74); RemapPixelBatch(ctx.dstbuf,BuildDarkeningLUT(0.5),{{x=0,y=0,w=1,h=1}}); return true",true,false,
                [](Buffers& b){b.D(0,0)=5;}},
        };
        for(const auto& item:cases)Run(item);
        std::printf("{\"schema\":1,\"cases\":%zu,\"passed\":true,\"scope\":\"original Lua callback and buffer APIs; manager fallback and live routing require separate integration tests\"}\n",cases.size());
        return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
