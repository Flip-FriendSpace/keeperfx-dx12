// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Owner-colour coverage, independent of the indexed palette compositor.
Texture2D<float4> coverage_atlas : register(t0);
Texture2D<float4> colour_rows : register(t1);
cbuffer SpriteFrame : register(b0) { float4 palette_rows[3]; };
struct VertexInput {
    float4 position : POSITION;
    float2 local : TEXCOORD0;
    int4 mapping : TEXCOORD1;
    uint4 source : TEXCOORD2;
    uint4 parameters : TEXCOORD3; // content height, horizontal flip, pass kind, premultiplied
    float4 colour : COLOR;
    uint2 lookup : TEXCOORD4; // Resolved row, single-texture fallback.
    float source_alpha : TEXCOORD5;
};
struct PixelInput {
    float4 position : SV_Position;
    noperspective float2 local : TEXCOORD0;
    nointerpolation int4 mapping : TEXCOORD1;
    nointerpolation uint4 source : TEXCOORD2;
    nointerpolation uint4 parameters : TEXCOORD3;
    nointerpolation float4 colour : COLOR;
    nointerpolation uint2 lookup : TEXCOORD4;
    nointerpolation float source_alpha : TEXCOORD5;
};
PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    float4 clip = float4(input.position.xyz * input.position.w, input.position.w);
    output.position = float4(clip.xy, (clip.z + clip.w) * 0.5, clip.w);
    output.local = input.local;
    output.mapping = input.mapping;
    output.source = input.source;
    output.parameters = input.parameters;
    output.colour = input.colour;
    output.lookup = input.lookup;
    output.source_alpha = input.source_alpha;
    return output;
}
bool Covered(PixelInput input, int2 sample_pixel)
{
    // Full logical bounds, not content-height or uninitialised atlas padding.
    if (any(sample_pixel < 0) || any(sample_pixel >= int2(input.source.zw))) return false;
    return coverage_atlas.Load(int3(input.source.xy + uint2(sample_pixel), 0)).g >= 0.5;
}
float4 PSMain(PixelInput input) : SV_Target
{
    // Same integer sampling as KSPR_SOURCE_TEXEL_GLSL. Bounds validation keeps
    // the shift/subtraction representable; phase and step are resolved inputs.
    int2 q = int2(floor(input.local));
    int2 j = max(((q + 1) << 16) - input.mapping.xy - 1, 0) / max(input.mapping.zw, 1);
    int2 last = int2(input.source.z, input.parameters.x) - 1;
    j = min(j, last);
    if (input.parameters.y != 0) j.x = last.x - j.x;
    if (input.parameters.z >= 4) {
        // Production glow ignores coverage; holes decode to raw index zero.
        uint index = (uint)round(coverage_atlas.Load(int3(input.source.xy + uint2(j), 0)).r * 255.0);
        if (index < 1 || index > 64) discard;
        uint code = index - 1, family = code / 8, strength = code % 8;
        bool darken = input.parameters.z == 5;
        if (strength == 0 || (family == 6) != darken) discard;
        static const float3 steps[8] = {
            float3(16,16,16)/255.0, float3(24,16,0)/255.0,
            float3(24,4,4)/255.0, float3(8,8,24)/255.0,
            float3(8,24,8)/255.0, float3(12,0,12)/255.0,
            float3(8,8,8)/255.0, float3(24,12,4)/255.0
        };
        float3 step = steps[family] * float(strength);
        float3 delta = saturate(float3(dot(palette_rows[0].xyz, step),
            dot(palette_rows[1].xyz, step), dot(palette_rows[2].xyz, step)));
        return float4(delta, input.parameters.w != 0 || darken ? 0.0 : 1.0);
    }
    if (!Covered(input, j)) discard;
    if (input.parameters.z == 3) {
        uint index = (uint)round(coverage_atlas.Load(int3(input.source.xy + uint2(j), 0)).r * 255.0);
        float4 c = colour_rows.Load(int3(index, input.lookup.x, 0));
        float a = (input.lookup.y != 0 ? 1.0 : c.a) * input.source_alpha;
        return float4(input.parameters.w != 0 ? c.rgb * a : c.rgb, a);
    }
    if (input.parameters.z == 1 && Covered(input, j + int2(-1, 0)) && Covered(input, j + int2(1, 0))
        && Covered(input, j + int2(0, -1)) && Covered(input, j + int2(0, 1))) discard;
    return input.parameters.w != 0 ? float4(input.colour.rgb * input.colour.a, input.colour.a) : input.colour;
}
