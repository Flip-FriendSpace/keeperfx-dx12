// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
Texture2D<uint> source_indices : register(t0);
Texture2D<uint4> source_palette : register(t1);
RWTexture2D<float4> target : register(u0);
cbuffer IndexedFrame : register(b0) {
    uint2 native_extent;
    uint2 source_origin;
    uint2 source_extent;
    uint clear_index;
    uint unused;
};
[numthreads(8,8,1)]
void CSMain(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= native_extent)) return;
    uint index = clear_index;
    if (all(thread.xy >= source_origin) && all(thread.xy - source_origin < source_extent))
        index = source_indices.Load(int3(thread.xy - source_origin, 0));
    // Both zero and255 are literal opaque palette entries. No CPU expansion,
    // implicit current palette, coverage mask, or RGB-to-index conversion.
    target[thread.xy] = float4(source_palette.Load(int3(index,0,0))) / 255.0;
}
