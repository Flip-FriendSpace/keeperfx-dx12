// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
cbuffer PresentRect : register(b0) { float4 destination; uint2 source_size; uint2 padding; };
Texture2D<float4> Source : register(t0);
float4 PSMain(float4 position : SV_Position) : SV_Target {
    float2 local = position.xy - destination.xy;
    if (any(local < 0) || any(local >= destination.zw)) return float4(0,0,0,1);
    uint2 texel = min(uint2(local * float2(source_size) / destination.zw), source_size - 1);
    return float4(Source.Load(int3(texel,0)).rgb,1);
}
