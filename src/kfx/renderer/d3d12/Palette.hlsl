// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Exact palette operators. This pass owns indices, never reconstructed RGB.
Texture2D<float4> source_atlas : register(t0);
#ifdef ORDERED_VIEWS
RasterizerOrderedTexture2D<uint> destination_indices : register(u0);
#else
RWTexture2D<uint> destination_indices : register(u0);
#endif
struct VertexInput {
    float2 position : POSITION;
    int2 origin : TEXCOORD0;
    uint2 extent : TEXCOORD1;
    uint4 source : TEXCOORD2;
    uint mode : TEXCOORD3;
    uint3 mapping : TEXCOORD4;
    uint2 lookup : TEXCOORD5;
    float3 terrain : TEXCOORD6;
    float4 sample_bounds : TEXCOORD7;
};
struct PixelInput {
    float4 position : SV_Position;
    nointerpolation int2 origin : TEXCOORD0;
    nointerpolation uint2 extent : TEXCOORD1;
    nointerpolation uint4 source : TEXCOORD2;
    nointerpolation uint mode : TEXCOORD3;
    nointerpolation uint3 mapping : TEXCOORD4;
    nointerpolation uint2 lookup : TEXCOORD5;
    noperspective float3 terrain : TEXCOORD6;
    nointerpolation float4 sample_bounds : TEXCOORD7;
};
PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.position = float4(input.position, 0.5, 1.0);
    output.origin = input.origin;
    output.extent = input.extent;
    output.source = input.source;
    output.mode = input.mode;
    output.mapping = input.mapping;
    output.lookup = input.lookup;
    output.terrain = input.terrain;
    output.sample_bounds = input.sample_bounds;
    return output;
}
void StoreIndex(uint2 pixel, uint value)
{
    // Preserve a diagnostic error bit through later opaque writes. This avoids
    // hiding an out-of-valid shadow sample beneath a later painter command.
    destination_indices[pixel] = (destination_indices[pixel] & ~255u) | value;
}
void PSMain(PixelInput input)
{
    if (input.mode == 6) {
        uint2 pixel = uint2(input.position.xy);
        // Floating interpolation at a covered edge can extrapolate just below
        // a zero endpoint and wrap to 255. Restore the triangle's convex
        // attribute range, not the mask range: genuinely invalid input remains
        // invalid, and intentionally negative coordinates still wrap normally.
        float2 uv = clamp(input.terrain.xy, input.sample_bounds.xy, input.sample_bounds.zw);
        uint2 sample_pixel = uint2(int2(floor(uv)) & 255);
        if (any(sample_pixel >= input.source.zw)) {
            StoreIndex(pixel, (destination_indices[pixel] & 255u) | 256u);
            return;
        }
        uint mask = (uint)round(source_atlas.Load(int3(input.source.xy + sample_pixel, 0)).r * 255.0);
        if (mask == 0) return;
        uint destination = destination_indices[pixel] & 255u;
        uint shade = (uint)input.terrain.z;
        uint result = (uint)round(source_atlas.Load(int3(input.lookup + uint2(destination, shade), 0)).r * 255.0);
        StoreIndex(pixel, result);
        return;
    }
    if (input.mode == 5 || input.mode == 7) {
        // The GPU rasterizes original projected geometry. These affine samples
        // deliberately do not emulate the original C scanline rounding.
        float2 uv = input.mapping.x != 0
            ? clamp(input.terrain.xy, input.sample_bounds.xy, input.sample_bounds.zw) : input.terrain.xy;
        int2 tile_pixel = int2(floor(uv)) & int2(input.source.zw - 1);
        uint index = (uint)round(source_atlas.Load(int3(input.source.xy + uint2(tile_pixel), 0)).r * 255.0);
        uint result = index;
        if (input.mode == 5) {
            uint shade = (uint)clamp(floor(input.terrain.z), 0.0, 63.0);
            result = (uint)round(source_atlas.Load(int3(input.lookup + uint2(index, shade), 0)).r * 255.0);
        }
        StoreIndex(uint2(input.position.xy), result);
        return;
    }
    // Exact pixel-centre sampling, including integer-boundary ties. Interpolated
    // floats can drift below a boundary and select a neighbouring source texel.
    uint2 local = uint2(int2(input.position.xy) - input.origin);
    uint2 source_pixel;
    if (input.mapping.x != 0) {
        uint2 xbytes = (uint2)round(source_atlas.Load(int3(local.x, input.mapping.y, 0)).rg * 255.0);
        uint2 ybytes = (uint2)round(source_atlas.Load(int3(local.y, input.mapping.z, 0)).rg * 255.0);
        uint sx = xbytes.x | (xbytes.y << 8);
        uint sy = ybytes.x | (ybytes.y << 8);
        if (sx == 0 || sy == 0) return;
        source_pixel = input.source.xy + uint2(sx - 1, sy - 1);
    } else {
        source_pixel = input.source.xy + ((2 * local + 1) * input.source.zw) / (2 * input.extent);
    }
    float4 texel = source_atlas.Load(int3(source_pixel, 0));
    // Coverage is separate from the index: opaque palette zero must draw.
    if (texel.a < 0.5) return;
    uint source = (uint)round(texel.r * 255.0);
    uint2 pixel = uint2(input.position.xy);
    uint result = source;
    if (input.mode == 3) {
        result = (uint)round(source_atlas.Load(int3(input.lookup + uint2(source, 0), 0)).r * 255.0);
    } else if (input.mode != 0) {
        uint destination = destination_indices[pixel] & 255u;
        uint2 operands = (input.mode == 1 || input.mode == 4) ? uint2(destination, source) : uint2(source, destination);
        result = (uint)round(source_atlas.Load(int3(input.lookup + operands, 0)).r * 255.0);
    }
    StoreIndex(pixel, result);
}
