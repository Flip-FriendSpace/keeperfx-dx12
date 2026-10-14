// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Sealed producer commands and immutable source leases. No capture or GPU image input.
#pragma once
#include "RuntimeAssets.h"
#include "EnhancedWorldPass.h"
#include "WorldOverlayPass.h"
#include "LensPass.h"
#include <optional>

namespace kfx::render::dx12 {
struct RuntimeWorldTerrain {
    std::array<EnhancedTerrainVertex, 3> vertices{};
    // Empty means the existing missing-texture checkerboard, never hidden geometry.
    RuntimeAssetLease tile;
};
struct RuntimeWorldShadow {
    EnhancedShadowQuad draw;
    RuntimeAssetLease source;
};
struct RuntimeWorldSprite {
    OutlinePassDraw draw;
    RuntimeAssetLease source, colour_row;
};
struct RuntimeWorldOverlay {
    WorldOverlayQuad draw;
    RuntimeAssetLease source, colour_row;
};
// Exact post-world RenderGraph order. UI quads retain native coordinates;
// image modes use a complete original-dimension Indexed8 source and palette.
enum class RuntimePostPhase : uint8_t { ImagePresents, OverheadMap, GameUI, ParchmentWorld, ParchmentFlat, ParchmentUI, ParchmentFront, ZoomBoxes, FrontOverlay, Text, ImagePresentOverlay, Cursor };
enum class RuntimePostKind : uint8_t { Quad, ImageOpaque, ImageComposite, ImageTransparent, ImageCoverage, ImageZoom, QuadOpaque };
struct RuntimePostDraw {
    RuntimePostPhase phase=RuntimePostPhase::GameUI;
    RuntimePostKind kind=RuntimePostKind::Quad;
    WorldOverlayQuad draw; // FlatWorld/depth off; source and row references0.
    RuntimeAssetLease source,colour_row,coverage;
    std::array<float,5> zoom{}; // Map XY, screen XY, scale; ImageZoom only.
};
// Resolved lens state: animation/callback evaluation happens once at production.
// Remap pixels retain little-endian unsigned XY bits, including65535 sentinels.
struct RuntimeWorldLens {
    LensPassKind kind = LensPassKind::Copy;
    int32_t source_x = 0; // Native source row zero, matching normal GL composition.
    int32_t destination_x = 0, destination_y = 0;
    uint32_t viewport_width = 0, viewport_height = 0;
    std::array<float,4> mist_offsets{};
    uint8_t mist_lightness = 0;
    uint16_t overlay_alpha = 0;
    std::array<float,4> clear_rgba{0,0,0,1};
    RuntimeAssetLease palette, fade, rgb6, density, remap, overlay;
};
// Immutable captures are keyed by runtime epoch plus producer transition ID.
// Only a new ID with capture_pending creates them; repeated packets reuse them.
struct RuntimeWorldMapFade {
    uint64_t transition_id=0;
    bool capture_pending=false;
    float step=0,warp_scale=1;
    RuntimeAssetLease palette,fade,rgb6,ghost_table;
};
struct RuntimeWorldFrame {
    uint32_t native_width = 0, native_height = 0;
    int32_t viewport_x = 0, viewport_y = 0;
    uint32_t viewport_width = 0, viewport_height = 0;
    std::array<float, 4> clear_rgba{0, 0, 0, 1};
    std::array<float, 9> shade_settings{0, 0, 1, 1, 0, 1, 1, 0.4f, 0};
    std::array<float, 9> palette_transform{1, 0, 0, 0, 1, 0, 0, 0, 1};
    float time = 0;
    EnhancedFadeAddressing fade_addressing = EnhancedFadeAddressing::Sampler;
    RuntimeAssetLease palette, fade, lightmap;
    std::vector<RuntimeWorldTerrain> terrain;
    std::vector<RuntimeWorldShadow> shadows;
    // Exact producer phase/painter order. No sorting by resource or object identity.
    std::vector<RuntimeWorldSprite> sprites;
    // Native-space overlays follow sprites, preserving phase and painter order.
    std::vector<RuntimeWorldOverlay> overlays;
    // Lens reads the completed world and overlays; output outside its viewport is clear_rgba.
    std::optional<RuntimeWorldLens> lens;
    std::optional<RuntimeWorldMapFade> map_fade;
    std::vector<RuntimePostDraw> post_composition;
    // Native-space hands precede ordinary Cursor quads; depth is disabled.
    std::vector<RuntimeWorldSprite> cursor_sprites;
};
// TileR8 is 32x32. SpriteRGBA8 owns every declared texel (R=index, G=binary
// coverage), including initialized padding; dimensions are at most 256x256.
// ColourRowRGBA8 is 256x1, retaining actual uploaded alpha. FadeR8 is 256x64;
// LightmapR16 is 511x511 tightly packed little-endian uint16. Source coordinates
// in draws are lease-local, and their declared rectangle must fit that lease.
// Packet copies retain their asset leases, including cached producer packets.
// Region addresses remain stable while any caller or accepted GPU reader owns
// the version. Discard obsolete packets to permit reclamation; a newer
// publication invalidates old admission but does not release those owners.
// NormalSprite uses colour_row and lookup_row=0; all other passes require an
// empty colour_row and NoSpriteColourRow. Terrain/shadow/sprite geometry uses
// the same viewport-local coordinates as the qualified enhanced render core.
// Output is native-sized, cleared to clear_rgba outside the world viewport.
// Overlay sources use fully initialized Indexed8 at their original dimensions;
// normalized UVs retain their meaning. Non-solid draw.source is0, solid uses
// NoWorldOverlayResource with an empty lease. Sprite/Fade/Clut use colour_row0
// with a ColourRowRGBA8 lease; Solid/Colored use NoWorldOverlayResource and no row.
// Overlay positions/scissors are native-space, independent of world viewport.
// Lens auxiliary images are immutable leases; source/target are native-sized GPU surfaces.
// Post composition includes image presents, ordinary UI/text/front overlay and
// cursor. Map-fade captures same-packet world/parchment once per transition.
// PiP, screenshot and scRGB lifting remain unsupported.
}
