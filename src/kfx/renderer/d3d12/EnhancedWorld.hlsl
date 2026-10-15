// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Enhanced world colour stages. The sprite/outline/glow stages retain their
// own production mapping and blend operators on the same colour/depth target.
Texture2D<float> tile_atlas : register(t0);
Texture2D<float4> palette : register(t1);
Texture2D<uint> lightmap : register(t2);
Texture2D<float> fade_table : register(t3);
Texture2D<float4> sprite_atlas : register(t4);
SamplerState nearest_clamp : register(s0);
cbuffer WorldState : register(b0) {
    float4 shade0; // fullbright, ambient, scale, gamma
    float4 shade1; // lighting mode, darkness mode, fog speed, fog density
    float4 shade2; // filter, time, diagnostic explicit fade addressing, unused
    float4 viewport; // width, height, native x, native height - native y
};
struct TerrainInput {
    float4 position : POSITION;
    float2 uv : TEXCOORD0;
    float shade : TEXCOORD1;
    float2 stl : TEXCOORD2;
    uint tile : TEXCOORD3;
};
struct TerrainPixel {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float shade : TEXCOORD1;
    float2 stl : TEXCOORD2;
    nointerpolation uint tile : TEXCOORD3;
    float vertex_light : TEXCOORD4;
};
TerrainPixel TerrainVS(TerrainInput input)
{
    TerrainPixel output;
    float w = max(input.position.w, 1.0);
    float4 clip = float4(input.position.xyz * w, w);
    output.position = float4(clip.xy, (clip.z + clip.w) * 0.5, clip.w);
    output.uv = input.uv; output.shade = input.shade; output.stl = input.stl;
    output.tile = input.tile; output.vertex_light = 0;
    if (shade1.x == 1 && all(input.stl >= 0))
        output.vertex_light = float(lightmap.Load(int3(clamp(int2(input.stl), 0, 510), 0))) / 8192.0;
    return output;
}
float SampleLightmap(float2 position)
{
    float2 p = clamp(position, 0, 510);
    int2 lo = int2(floor(p)), hi = min(lo + 1, 510);
    float2 f = frac(p);
    float a = float(lightmap.Load(int3(lo, 0)));
    float b = float(lightmap.Load(int3(hi.x, lo.y, 0)));
    float c = float(lightmap.Load(int3(lo.x, hi.y, 0)));
    float d = float(lightmap.Load(int3(hi, 0)));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y) / 8192.0;
}
float Hash21(float2 p)
{
    p = frac(p * float2(123.34,456.21));
    p += dot(p,p+45.32);
    return frac(p.x*p.y);
}
float Noise2d(float2 p)
{
    float2 i=floor(p),f=frac(p); f=f*f*(3.0-2.0*f);
    return lerp(lerp(Hash21(i),Hash21(i+float2(1,0)),f.x),
        lerp(Hash21(i+float2(0,1)),Hash21(i+float2(1,1)),f.x),f.y);
}
float Fbm(float2 p)
{
    float v=0,a=0.5;
    for(int i=0;i<3;++i){v+=a*Noise2d(p);p*=2.0;a*=0.5;}
    return v;
}
float TileIndex(uint tile, int2 texel)
{
    // Origins stay on the32-texel atlas grid. Runtime references additionally
    // carry a resolution code; old diagnostic/32x32 references have code0.
    uint address=tile&0xffffff,dimension=32u<<((tile>>24)&3u);
    int2 origin=int2(address%128,address/128)*32;
    return tile_atlas.Load(int3(origin+(texel&int(dimension-1)),0));
}
float4 Decode(float index, float fade_v)
{
    if(shade1.y!=0){
        // The opt-in path specifies floor addressing without the sampler's
        // subtexel snap. Keep the same interpolated shade, normalization and
        // horizontal index coordinate; no epsilon or row bias is introduced.
        if(shade2.z==1)
            index=fade_table.Load(int3(clamp(int2(floor(float2(index,fade_v)*float2(256,64))),int2(0,0),int2(255,63)),0));
        else index=fade_table.SampleLevel(nearest_clamp,float2(index,fade_v),0);
    }
    return palette.SampleLevel(nearest_clamp,float2(index,0.5),0);
}
float4 TerrainPS(TerrainPixel input) : SV_Target
{
    float2 gl_fragment=float2(input.position.x+viewport.z,viewport.w-input.position.y);
    if(input.tile==0xffffffff){
        int2 cell=int2(gl_fragment/8.0);
        return ((cell.x+cell.y)&1)==0?float4(1,0,1,1):float4(0,0,0,1);
    }
    float raw=input.shade;
    if(shade1.x==1 && all(input.stl>=0) && input.vertex_light>0.0001)
        raw*=SampleLightmap(input.stl)/input.vertex_light;
    raw=lerp(raw,1.0,shade0.x);
    float fade_v=(clamp(raw*32.0,0.0,63.0)+0.5)/64.0;
    float4 colour;
    float2 terrain_uv=input.uv*float(1u<<((input.tile>>24)&3u));
    if(shade2.x==1){
        float2 px=terrain_uv-0.5,f=frac(px); int2 base=int2(floor(px));
        float4 a=Decode(TileIndex(input.tile,base),fade_v);
        float4 b=Decode(TileIndex(input.tile,base+int2(1,0)),fade_v);
        float4 c=Decode(TileIndex(input.tile,base+int2(0,1)),fade_v);
        float4 d=Decode(TileIndex(input.tile,base+int2(1,1)),fade_v);
        colour=lerp(lerp(a,b,f.x),lerp(c,d,f.x),f.y);
    }else colour=Decode(TileIndex(input.tile,int2(floor(terrain_uv))),fade_v);
    if(shade1.y==1)return float4(colour.rgb,1);
    if(shade1.y==2){
        float mask=1.0-saturate(raw);
        if(mask>0.01){
            float2 fog_uv=gl_fragment*0.008+float2(shade2.y*shade1.z*0.4,shade2.y*shade1.z*0.25);
            float alpha=smoothstep(0.25,0.65,Fbm(fog_uv))*mask*shade1.w;
            return float4(lerp(colour.rgb,float3(0.06,0.10,0.06),alpha),1);
        }
        return float4(colour.rgb,1);
    }
    float shade=pow(saturate(max(raw,shade0.y)*shade0.z),shade0.w);
    return float4(colour.rgb*shade,1);
}
struct ShadowInput {
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float2 depth_darken : TEXCOORD1;
    uint4 source : TEXCOORD2;
};
struct ShadowPixel {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation float darken : TEXCOORD1;
    nointerpolation uint4 source : TEXCOORD2;
};
ShadowPixel ShadowVS(ShadowInput input)
{
    ShadowPixel output;
    float2 ndc=float2(input.position.x/viewport.x*2.0-1.0,1.0-input.position.y/viewport.y*2.0);
    output.position=float4(ndc,(input.depth_darken.x+1.0)*0.5,1);
    output.uv=input.uv;output.darken=input.depth_darken.y;output.source=input.source;
    return output;
}
float4 ShadowPS(ShadowPixel input) : SV_Target
{
    // UVs retain pixel/256 units on both 256 and 512 coverage layers. Logical
    // source padding is explicitly owned zero; this is not software UV wrap.
    int2 p=clamp(int2(floor(input.uv*256.0)),0,511);
    if(any(p>=int2(input.source.zw)))discard;
    if(sprite_atlas.Load(int3(input.source.xy+uint2(p),0)).g<0.5)discard;
    float keep=1.0-input.darken;
    return float4(keep,keep,keep,1);
}
