// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Production GL UI shader semantics over owned executed world-overlay inputs.
Texture2D<float> IndexedSource : register(t0);
Texture2D<float4> ColourRows : register(t1);
Texture2D<float> FadeTable : register(t2);
SamplerState PointClamp : register(s0);
cbuffer OverlayConstants : register(b0)
{
    float2 NativeSize;
    uint Mode;
    uint ColourRow;
    uint FadeRow;
    uint3 Reserved;
    float2 ZoomMap;
    float2 ZoomScreen;
    float ZoomScale;
    uint3 ZoomReserved;
};
struct VertexInput {
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 colour : COLOR0;
    float z : TEXCOORD1;
};
struct PixelInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 colour : COLOR0;
};
PixelInput WorldOverlayVS(VertexInput input)
{
    PixelInput output;
    // Preserve original divide-then-multiply GL arithmetic and native origin.
    precise float x=(input.position.x/NativeSize.x)*2.0-1.0;
    precise float y=1.0-(input.position.y/NativeSize.y)*2.0;
    output.position=float4(x,y,(input.z+1.0)*0.5,1.0);
    output.uv=input.uv;
    output.colour=input.colour;
    return output;
}
float4 WorldOverlayPS(PixelInput input) : SV_Target
{
    if(Mode==2) return input.colour; // Solid: no source or palette access.
    if(Mode==8) {
        // Native SV_Position is the pixel centre, including the half-pixel.
        precise float2 source=ZoomMap+(input.position.xy-ZoomScreen)*ZoomScale;
        float2 texel=floor(source);
        uint width,height;IndexedSource.GetDimensions(width,height);
        if(any(texel<0) || texel.x>=width || texel.y>=height)return float4(0,0,0,1);
        uint index=(uint)(IndexedSource.Load(int3((int2)texel,0))*255.0+0.5);
        return float4(ColourRows.Load(int3(index,ColourRow,0)).rgb,1);
    }
    float sampled=IndexedSource.SampleLevel(PointClamp,input.uv,0);
    // Post-world indexed image presents share the exact native quad core.
    // Modes5/6/7 are opaque, index-zero transparent, explicit coverage.
    if(Mode>=5) {
        if(Mode==6 && sampled<(0.5/255.0)) discard;
        if(Mode==7 && FadeTable.SampleLevel(PointClamp,input.uv,0)<0.5) discard;
        uint index=(uint)(sampled*255.0+0.5);
        return float4(ColourRows.Load(int3(index,ColourRow,0)).rgb,1);
    }
    if(sampled<(0.5/255.0)) discard; // Original source zero, before any lookup.
    if(Mode==1) return input.colour; // Colored: source is only a nonzero mask.
    uint index=(uint)(sampled*255.0+0.5);
    if(Mode==3) index=(uint)(FadeTable.Load(int3(index,FadeRow,0))*255.0+0.5);
    float4 row=ColourRows.Load(int3(index,ColourRow,0));
    float alpha=Mode==4?row.a*input.colour.a:input.colour.a;
    return float4(row.rgb*input.colour.rgb,alpha);
}
