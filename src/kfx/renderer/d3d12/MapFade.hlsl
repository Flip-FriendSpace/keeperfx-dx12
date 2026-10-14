// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Legacy GL MAPFADE_FRAGMENT_SHADER, native top-down captures.
// Shares the lens root (seven SRVs, eighty bytes of constants).
Texture2D<float4> world_image : register(t0);
Texture2D<float4> palette : register(t1);
Texture2D<float> fade_table : register(t2);
Texture2D<float> rgb6 : register(t3);
Texture2D<float> ghost : register(t4);
Texture2D<float4> parchment_image : register(t5);
cbuffer MapFadeState : register(b0) {
    uint4 source_destination;
    uint4 rectangle_kind;
    uint4 resources;
    int4 address;
    float4 parameters; // step, warp strength, unused
};
int Index(float3 rgb) {
    int3 c=int3(saturate(rgb)*63.0+0.5);
    int k=(c.r<<12)|(c.g<<6)|c.b;
    return int(rgb6.Load(int3(k&511,k>>9,0))*255.0+0.5);
}
float4 MapFadePS(float4 position : SV_Position) : SV_Target {
    precise float2 uv=position.xy/float2(source_destination.xy);
    precise float wp=(32.0-parameters.x)*parameters.y;
    precise float ww=parameters.x*parameters.y;
    precise float2 puv=float2(uv.x+wp*(4.0-8.0*uv.x)/320.0,uv.y+wp*4.0*(1.0-2.0*uv.y)/320.0);
    precise float2 wuv=float2(uv.x+ww*(4.0-8.0*uv.x)/320.0,uv.y+ww*4.0*(1.0-2.0*uv.y)/320.0);
    // At strength 40 the legacy expression mathematically collapses to the
    // centre. Define that point explicitly: reciprocal rounding otherwise
    // produces implementation-dependent stripes across adjacent centre texels.
    if (wp == 40.0) puv = float2(0.5,0.5);
    if (ww == 40.0) wuv = float2(0.5,0.5);
    const int2 size=int2(source_destination.xy);
    // GL captures are bottom-up and fetch at (x,1-y). Convert the chosen
    // integer texel, preserving its preceding-row choice at exact boundaries.
    int2 p=clamp(int2(float2(saturate(puv.x),1.0-saturate(puv.y))*size),int2(0,0),size-1);
    int2 w=clamp(int2(float2(saturate(wuv.x),1.0-saturate(wuv.y))*size),int2(0,0),size-1);
    p.y=size.y-1-p.y;
    w.y=size.y-1-w.y;
    int row=clamp(int(parameters.x+0.5),0,32);
    int pi=int(fade_table.Load(int3(Index(parchment_image.Load(int3(p,0)).rgb),row,0))*255.0+0.5);
    int wi=int(fade_table.Load(int3(Index(world_image.Load(int3(w,0)).rgb),32-row,0))*255.0+0.5);
    int result=int(ghost.Load(int3(pi,wi,0))*255.0+0.5);
    return float4(palette.Load(int3(result,0,0)).rgb,1);
}
