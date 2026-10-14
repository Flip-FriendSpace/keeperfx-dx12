// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// A changed clear colour must not require replacing a reusable frame target.
cbuffer ClearState : register(b0) { float4 clear_colour; };
float4 VSMain(uint vertex : SV_VertexID) : SV_Position
{
    float2 corner = float2((vertex << 1) & 2, vertex & 2);
    return float4(corner * 2.0 - 1.0, 0, 1);
}
float4 PSMain() : SV_Target { return clear_colour; }
