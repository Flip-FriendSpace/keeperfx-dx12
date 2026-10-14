// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Expand authoritative GPU indices without reconstructing indices from colour.
Texture2D<uint> indices : register(t0);
Buffer<uint> palette : register(t1); // Packed actual eight-bit R, G, B bytes.
RWTexture2D<float4> colour : register(u0);
RWByteAddressBuffer error_status : register(u1);

[numthreads(8, 8, 1)]
void CSMain(uint3 thread : SV_DispatchThreadID)
{
    uint width, height;
    indices.GetDimensions(width, height);
    if (thread.x >= width || thread.y >= height) return;
    uint index = indices.Load(int3(thread.xy, 0));
    if (index > 255) {
        uint ignored;
        error_status.InterlockedOr(0, 1, ignored);
        colour[thread.xy] = float4(1, 0, 1, 1);
        return;
    }
    uint packed = palette[index];
    uint3 channels = uint3(packed & 255, (packed >> 8) & 255, (packed >> 16) & 255);
    colour[thread.xy] = float4(float3(channels) / 255.0, 1);
}
