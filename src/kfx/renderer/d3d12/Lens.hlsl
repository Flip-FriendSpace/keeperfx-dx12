// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Standard enhanced lens operators corresponding to the production LENS_*
// shaders in opengl/GLShaders.h. Inputs are actual owned upload bytes. These
// operators intentionally include GL's RGB6 recovery for indexed effects.
Texture2D<float4> lens_source : register(t0);
Texture2D<float4> lens_palette : register(t1);
Texture2D<float> lens_fade : register(t2);
Texture2D<float> lens_rgb6 : register(t3);
Texture2D<float> lens_density : register(t4);
Texture2D<uint2> lens_remap : register(t5);
Texture2D<float> lens_overlay : register(t6);
cbuffer LensState : register(b0) {
    uint4 source_destination;
    uint4 rectangle_kind;
    uint4 resources;
    int4 address;
    float4 mist_offsets;
};

// Caller binds the exact destination viewport/scissor on the native target,
// disables blending/depth, and retains the source texture until fence completion.
float4 LensVS(uint vertex : SV_VertexID) : SV_Position
{
    const float2 corners[3] = {float2(-1,1),float2(3,1),float2(-1,-3)};
    return float4(corners[vertex],0,1);
}
float3 Scene(int2 pixel)
{
    const int2 native_pixel = clamp(pixel + address.xy, int2(0,0), int2(source_destination.xy)-1);
    return lens_source.Load(int3(native_pixel,0)).rgb;
}
int PaletteIndex(float3 rgb)
{
    const int3 colour = int3(saturate(rgb)*63.0+0.5);
    const int key = (colour.r<<12)|(colour.g<<6)|colour.b;
    return int(lens_rgb6.Load(int3(key&511,key>>9,0))*255.0+0.5);
}
float3 PaletteColour(int index)
{
    return lens_palette.Load(int3(index,0,0)).rgb;
}
float4 LensPS(float4 position : SV_Position) : SV_Target
{
    // SV_Position uses native top-down pixel centres. Unlike source_y, the
    // destination Y offset is subtracted here; source_y remains exactly zero.
    const int2 size = int2(rectangle_kind.xy);
    const int2 d = clamp(int2(position.xy)-int2(source_destination.zw),int2(0,0),size-1);
    const uint kind = rectangle_kind.z;
    if (kind == 1) {
        const int2 table_pixel = clamp(d,int2(0,0),int2(resources.xy)-1);
        const uint2 src = lens_remap.Load(int3(table_pixel,0));
        return float4(Scene(int2(src)),1);
    }
    if (kind == 2) {
        const int2 scale = int2(640<<16,480<<16)/size;
        const int2 v = (d*scale)>>16;
        const int4 offsets = int4(mist_offsets);
        const int2 primary = (offsets.xy+v)&255;
        const int2 secondary = int2(offsets.z+0x10000-v.y,offsets.w+0x10000-v.x)&255;
        const int k = int(lens_density.Load(int3(primary,0))*255.0+0.5);
        const int i = int(lens_density.Load(int3(secondary,0))*255.0+0.5);
        const int row = min(int(rectangle_kind.w)+min((k+i)>>3,32),63);
        const int index = int(lens_fade.Load(int3(PaletteIndex(Scene(d)),row,0))*255.0+0.5);
        return float4(PaletteColour(index),1);
    }
    if (kind == 3) {
        const float3 scene = Scene(d);
        const int2 overlay_size = int2(resources.zw);
        const int2 scale = (overlay_size<<16)/size;
        const int2 pixel = min((d*scale)>>16,overlay_size-1);
        const int ov = int(lens_overlay.Load(int3(pixel,0))*255.0+0.5);
        if (ov == 255) return float4(scene,1);
        const int index = (ov*address.z+PaletteIndex(scene)*(256-address.z))>>8;
        return float4(PaletteColour(index),1);
    }
    return float4(Scene(d),1);
}
