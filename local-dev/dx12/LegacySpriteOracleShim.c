// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Isolated state for the unmodified software rasterizer linked into the
// diagnostic executable. No display, renderer manager or simulation is linked.
#include "LegacySpriteOracleShim.h"
#include "bflib_vidraw.h"
#include "bflib_render.h"
#include "custom_sprites.h"
#include "kfx/renderer/RendererManager.h"
#include "kfx/renderer/software/SwDrawTarget.h"

static uint8_t* oracle_pixels;
static int32_t oracle_width;
static int32_t oracle_height;
static int32_t oracle_window_x, oracle_window_y, oracle_window_width, oracle_window_height;
static uint32_t oracle_guard;
static int oracle_native, oracle_copy_failure;
static int64_t oracle_copy_source, oracle_copy_destination;
static uint32_t oracle_copy_length;
static uint16_t oracle_flags;
unsigned char* render_ghost;
unsigned char* render_alpha;
// PE linkers can retain references from unused functions in the same original
// translation units. Satisfy those references without bringing in game/UI state.
// None are part of either replay path; fail immediately if that contract changes.
TbDisplayStruct lbDisplay;
TbResult RendererDrawBox(int32_t x, int32_t y, uint32_t width, uint32_t height, unsigned char colour)
{
    (void)x; (void)y; (void)width; (void)height; (void)colour;
    abort();
}
TbResult RendererSpriteDraw(int32_t x, int32_t y, const struct TbSprite* spr)
{
    (void)x; (void)y; (void)spr;
    abort();
}
TbResult RendererSpriteDrawOneColour(int32_t x, int32_t y, const struct TbSprite* spr, unsigned char colour)
{
    (void)x; (void)y; (void)spr; (void)colour;
    abort();
}
TbResult RendererSpriteDrawScaled(int32_t x, int32_t y, const struct TbSprite* spr, int32_t w, int32_t h)
{
    (void)x; (void)y; (void)spr; (void)w; (void)h;
    abort();
}
TbResult RendererSpriteDrawScaledOneColour(int32_t x, int32_t y, const struct TbSprite* spr,
    int32_t w, int32_t h, unsigned char colour)
{
    (void)x; (void)y; (void)spr; (void)w; (void)h; (void)colour;
    abort();
}
int RendererSpriteDrawScaledRemap(int32_t x, int32_t y, const struct TbSprite* spr,
    int32_t w, int32_t h, const unsigned char* cmap)
{
    (void)x; (void)y; (void)spr; (void)w; (void)h; (void)cmap;
    abort();
}
TbResult LbSpriteDrawOneColourUsingScalingData(long posx, long posy,
    const struct TbSprite* sprite, TbPixel colour)
{
    (void)posx; (void)posy; (void)sprite; (void)colour;
    abort();
}
const struct TbSprite* get_panel_sprite(short sprite_idx)
{
    (void)sprite_idx;
    abort();
}
TbResult CorrectedLbSpriteDrawUsingScalingUpDataSolidRL(uchar*, int, int,
    int32_t*, int32_t*, const struct TbSourceBuffer*);
TbResult CorrectedLbSpriteDrawRemapUsingScalingUpDataSolidRL(uchar*, int, int,
    int32_t*, int32_t*, const struct TbSourceBuffer*, const TbPixel*);

TbPixel* SwTargetWScreen(void) { return oracle_pixels; }
TbPixel* SwTargetGraphicsWindowPtr(void) { return oracle_pixels + oracle_window_y * oracle_width + oracle_window_x; }
int32_t SwTargetScanline(void) { return oracle_width; }
int32_t SwTargetScreenHeight(void) { return oracle_height; }
int32_t SwTargetWindowX(void) { return oracle_window_x; }
int32_t SwTargetWindowY(void) { return oracle_window_y; }
int32_t SwTargetWindowWidth(void) { return oracle_window_width; }
int32_t SwTargetWindowHeight(void) { return oracle_window_height; }
unsigned short RendererGetDrawFlags(void) { return oracle_flags; }

int LegacySpriteOracleXSteps(void) { return SPRITE_SCALING_XSTEPS; }
int LegacySpriteOracleYSteps(void) { return SPRITE_SCALING_YSTEPS; }
void LegacySpriteOracleTarget(uint8_t* pixels, int32_t width, int32_t height)
{
    oracle_pixels = pixels;
    oracle_width = width;
    oracle_height = height;
    oracle_window_x = oracle_window_y = 0;
    oracle_window_width = width; oracle_window_height = height;
    oracle_native = oracle_copy_failure = 0;
}
void LegacySpriteOracleNativeTarget(uint8_t* pixels, int32_t pitch, int32_t height,
    int32_t window_x, int32_t window_y, int32_t window_w, int32_t window_h, uint32_t guard)
{
    oracle_pixels = pixels; oracle_width = pitch; oracle_height = height;
    oracle_window_x = window_x; oracle_window_y = window_y;
    oracle_window_width = window_w; oracle_window_height = window_h;
    oracle_guard = guard; oracle_native = 1; oracle_copy_failure = 0;
}
int LegacySpriteOracleCopyFailure(int64_t* source, int64_t* destination, uint32_t* length)
{
    *source = oracle_copy_source; *destination = oracle_copy_destination; *length = oracle_copy_length;
    return oracle_copy_failure;
}
void __real_LbPixelBlockCopyForward(TbPixel* dst, const TbPixel* src, long len);
void __wrap_LbPixelBlockCopyForward(TbPixel* dst, const TbPixel* src, long len)
{
    if (oracle_native && len > 0) {
        const intptr_t origin = (intptr_t)SwTargetGraphicsWindowPtr();
        const intptr_t allocation = (intptr_t)oracle_pixels - oracle_guard;
        const intptr_t allocation_end = (intptr_t)oracle_pixels + (intptr_t)oracle_width * oracle_height + oracle_guard;
        const int64_t source = (intptr_t)src - origin;
        const int64_t destination = (intptr_t)dst - origin;
        int failure = 0;
        if ((intptr_t)dst < allocation || (intptr_t)dst > allocation_end - len) failure = 2;
        // Every copied source byte must be owned viewport state. Native UI,
        // sidebar and row padding were not captured, even if a clear preceded
        // the world. This also catches same-value and read-only accesses.
        int64_t at = source;
        long remaining = len;
        while (!failure && remaining > 0) {
            if (at < 0 || at / oracle_width >= oracle_window_height) { failure = 1; break; }
            const long x = (long)(at % oracle_width);
            const long row_count = remaining < oracle_width - x ? remaining : oracle_width - x;
            if (x + row_count > oracle_window_width) { failure = 1; break; }
            at += row_count; remaining -= row_count;
        }
        if (failure) {
            if (!oracle_copy_failure) {
                oracle_copy_failure = failure; oracle_copy_source = source;
                oracle_copy_destination = destination; oracle_copy_length = (uint32_t)len;
            }
            return;
        }
    }
    __real_LbPixelBlockCopyForward(dst, src, len);
}
void LegacySpriteOracleClear(void)
{
    oracle_pixels = NULL;
    oracle_width = oracle_height = 0;
    oracle_window_x = oracle_window_y = oracle_window_width = oracle_window_height = 0;
    oracle_native = oracle_copy_failure = 0;
    oracle_flags = 0;
    render_ghost = render_alpha = lbSpriteReMapPtr = NULL;
}
int LegacySpriteOracleDraw(const struct SpriteScale* scale,
    const uint8_t* rle, uint32_t source_width, uint32_t content_height,
    uint16_t flags, int alpha, int corrected, const uint8_t* remap,
    const uint8_t* ghost, const uint8_t* additive)
{
    oracle_copy_failure = 0;
    const struct TbSourceBuffer buffer = {
        rle, source_width, content_height, source_width
    };
    oracle_flags = flags;
    lbSpriteReMapPtr = (unsigned char*)remap;
    render_ghost = (unsigned char*)ghost;
    render_alpha = (unsigned char*)additive;
    // Exact entry points and parameters used by SoftwareWorldViewRenderer.
    LbSpriteSetScalingData(scale->frame_x, scale->frame_y,
        scale->frame_src_w, scale->frame_src_h,
        scale->frame_dst_w, scale->frame_dst_h);
    if (alpha)
        return DrawAlphaSpriteUsingScalingData(scale->content_x, scale->content_y, &buffer);
    if (corrected && scale_up && (flags & Lb_SPRITE_FLIP_HORIZ)
        && ((flags & Lb_SPRITE_REMAP) || !(flags & (Lb_SPRITE_TRANSPAR4 | Lb_SPRITE_TRANSPAR8))))
    {
        int32_t* xstep;
        int32_t* ystep;
        int scanline;
        uchar* outbuf;
        int outheight;
        setup_steps(scale->content_x, scale->content_y, &buffer, &xstep, &ystep, &scanline);
        setup_outbuf(xstep, ystep, &outbuf, &outheight);
        if (flags & Lb_SPRITE_REMAP)
            return CorrectedLbSpriteDrawRemapUsingScalingUpDataSolidRL(
                outbuf, scanline, outheight, xstep, ystep, &buffer, remap);
        return CorrectedLbSpriteDrawUsingScalingUpDataSolidRL(
            outbuf, scanline, outheight, xstep, ystep, &buffer);
    }
    return LbSpriteDrawUsingScalingData(scale->content_x, scale->content_y, &buffer);
}
