// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX remaster changes: 2026-10-16.
// Presentation-only coloured illumination contract. Not wired to gameplay lights.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
namespace kfx::render::lighting {
using RGB = std::array<float,3>;
struct Point { float x=0,y=0,z=0; };
struct Source {
    uint64_t epoch=0, incarnation=0;
    uint32_t slot=0;
    Point position;
    float radius=0, gain=0; // World subtiles; gain already sampled for this frame.
    RGB linear_rgb{}; // Explicit metadata; never inferred from bright pixels.
    bool revealed=false;
    uint64_t sampled_sequence=0;
};
struct Receiver {
    Point position;
    // Both terrain and sprites use the same response contract. Unknown normal
    // uses isotropic diffuse. Authored normals must already be in world space.
    Point normal{0,0,1};
    float baked_fraction=1; // Unknown/fully painted lighting suppresses relighting.
    bool revealed=false, ownership_protected=false, normal_authored=false;
    uint64_t identity=0;
};
struct FrameTag {
    uint64_t epoch=0, sequence=0, geometry_revision=0;
    bool operator==(const FrameTag&) const = default;
};
struct PairVisibility {
    FrameTag frame;
    uint64_t source_incarnation=0, receiver_identity=0;
    uint32_t source_slot=0;
    float fraction=0;
};
inline bool Finite(Point p) noexcept { return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z); }
inline float SmoothFalloff(float distance,float radius) noexcept {
    if (!std::isfinite(distance)||!std::isfinite(radius)||distance<0||radius<=0) return 0;
    const float t=std::clamp(distance/radius,0.0f,1.0f);
    return (1-t)*(1-t)*(1+2*t);
}
// Occlusion belongs to the exact source/receiver/frame pair. The caller must
// supply zero when visibility, geometry revision or source identity is stale.
// Requiring it explicitly prevents unoccluded coloured spill from the existing
// max-composed scalar map, which cannot identify each source's contribution.
inline RGB Irradiance(const Source& source,const Receiver& receiver,FrameTag frame,
    PairVisibility visibility) noexcept {
    RGB result{};
    if (!source.revealed||!receiver.revealed||receiver.ownership_protected||!source.incarnation
        ||!frame.epoch||!frame.sequence||!frame.geometry_revision||source.epoch!=frame.epoch
        ||source.sampled_sequence!=frame.sequence||!receiver.identity||visibility.frame!=frame
        ||visibility.source_incarnation!=source.incarnation||visibility.source_slot!=source.slot
        ||visibility.receiver_identity!=receiver.identity||!Finite(source.position)||!Finite(receiver.position)
        ||!std::isfinite(source.gain)||source.gain<0||source.gain>1
        ||!std::isfinite(visibility.fraction)||visibility.fraction<0||visibility.fraction>1) return result;
    for(float c:source.linear_rgb) if(!std::isfinite(c)||c<0||c>1) return result;
    const Point d{source.position.x-receiver.position.x,source.position.y-receiver.position.y,source.position.z-receiver.position.z};
    const float distance=std::sqrt(d.x*d.x+d.y*d.y+d.z*d.z);
    float weight=SmoothFalloff(distance,source.radius)*source.gain*visibility.fraction;
    if(receiver.normal_authored) {
        if(!Finite(receiver.normal)) return result;
        const auto n=receiver.normal;
        const float length=std::sqrt(n.x*n.x+n.y*n.y+n.z*n.z);
        if(!std::isfinite(length)||length<0.0001f) return result;
        if(distance>0.0001f)weight*=std::max(0.0f,(n.x*d.x+n.y*d.y+n.z*d.z)/(length*distance));
    }
    for(unsigned c=0;c<3;++c)result[c]=source.linear_rgb[c]*weight;
    return result;
}
// Input/output are linear RGB AFTER legacy palette/fade. Bounded headroom
// preserves existing painted shading; sources add only into remaining range.
inline RGB DiffuseResponse(RGB established,const Receiver& receiver,RGB irradiance,float strength) noexcept {
    if(!receiver.revealed||receiver.ownership_protected||!std::isfinite(strength)
        ||strength<0||strength>0.25f||!std::isfinite(receiver.baked_fraction)
        ||receiver.baked_fraction<0||receiver.baked_fraction>1)return established;
    for(unsigned c=0;c<3;++c)if(!std::isfinite(established[c])||established[c]<0||established[c]>1
        ||!std::isfinite(irradiance[c])||irradiance[c]<0)return established;
    const float gain=strength*(1-receiver.baked_fraction);
    for(unsigned c=0;c<3;++c)established[c]+=(1-established[c])*std::min(irradiance[c],1.0f)*gain;
    return established;
}
}
