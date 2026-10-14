// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
cbuffer Frame : register(b0)
{
    column_major float4x4 world_to_clip;
};
Texture2D<float4> surface_texture : register(t0);
SamplerState surface_sampler : register(s0);
struct VertexInput {
    float3 position : POSITION; float2 uv : TEXCOORD0;
    float shade : TEXCOORD1; float2 tile_origin : TEXCOORD2;
};
struct PixelInput {
    float4 position : SV_Position; float2 uv : TEXCOORD0;
    float shade : TEXCOORD1; nointerpolation float2 tile_origin : TEXCOORD2;
    noperspective float2 sprite_uv : TEXCOORD3;
};
PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.position = mul(world_to_clip, float4(input.position, 1.0));
    output.uv = input.uv;
    output.shade = input.shade;
    output.tile_origin = input.tile_origin;
    output.sprite_uv = input.uv;
    return output;
}
float4 PSMain(PixelInput input) : SV_Target
{
    if (input.shade >= 0) {
        // Tile-local coordinates and explicit texel loads keep adjacent atlas
        // tiles separate. Shade maps the original palette index through the
        // captured fade table before converting to the display colour.
        int2 texel = int2(input.tile_origin) + min(int2(saturate(input.uv) * 32), 31);
        int index = (int)round(surface_texture.Load(int3(texel, 0)).r * 255);
        int shade = clamp((int)floor(input.shade), 0, 63);
        return surface_texture.Load(int3(index, shade, 0));
    }
    // Upright sprite depth may vary with camera pitch, but its artwork keeps
    // the original screen-space sampling. Ordinary RGBA meshes stay perspective-correct.
    float4 colour = surface_texture.Sample(surface_sampler, input.shade < -1.5 ? input.sprite_uv : input.uv);
    clip(colour.a - 0.5); // Sprite coverage: transparent texels must not write depth.
    return colour;
}
