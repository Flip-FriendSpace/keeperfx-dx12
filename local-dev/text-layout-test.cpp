// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// CPU fixtures for the real shared layout, with deterministic synthetic fonts.
#include "pre_inc.h"
#include "kfx/renderer/TextLayout.h"
#include "kfx/renderer/ir/TextCommands.h"
#include "bflib_sprfnt.h"
#include "bflib_sprite.h"
#include "bflib_text.h"
#include "bflib_video.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include "post_inc.h"

using namespace kfx::render;
static TbSprite sprite{nullptr, 5, 7};
static int dbc_height = 16;
static int font_token;
extern "C" {
volatile TbDisplayStructEx lbDisplayEx{};
TbBool dbc_enabled = true, dbc_initialized = false;
const TbSpriteSheet* lbFontPtr = reinterpret_cast<const TbSpriteSheet*>(&font_token);
AsianFont* active_dbcfont = reinterpret_cast<AsianFont*>(&font_token);
long dbc_colour0 = 37, dbc_colour1 = 41;
unsigned char RendererGetDrawColour() { return 19; }
unsigned short RendererGetDrawFlags() { return Lb_TEXT_HALIGN_CENTER; }
const unsigned char* LbTextGetRemap() { static const unsigned char remap[256]{}; return remap; }
void LbTextGetJustifyWindow(int* x, int* y, int* w) { *x = 11; *y = 17; *w = 80; }
void LbTextGetClipWindow(int* x, int* y, int* w, int* h) { *x = 11; *y = 17; *w = 80; *h = 100; }
unsigned int LbTextGetFontGeneration() { return 12; }
int LbSprFontCharWidth(const TbSpriteSheet*, uint32_t chr) { return chr == ' ' ? 3 : 5; }
int LbSprFontCharHeight(const TbSpriteSheet*, uint32_t) { return 8; }
const TbSprite* LbFontCharSprite(const TbSpriteSheet*, uint32_t chr) { return chr == '?' ? nullptr : &sprite; }
unsigned char LbTextGetSpacesPerTab() { return 2; }
TbBool LbAlignMethodSet(unsigned short flags) { return (flags & (Lb_TEXT_HALIGN_LEFT | Lb_TEXT_HALIGN_RIGHT | Lb_TEXT_HALIGN_CENTER | Lb_TEXT_HALIGN_JUSTIFY)) != 0; }
long LbGetJustifiedCharPosY(long start, long, long, unsigned short) { return start; }
int LbDbcCharHeight(const AsianFont*) { return dbc_height + 4; }
int LbDbcCharWidthM(const AsianFont*, uint32_t, long units) { return 9 * units / 16; }
TbBool LbDbcIsDuospaceChar(const AsianFont*, uint32_t) { return false; }
int LbDbcGetGlyphBits(const AsianFont*, uint32_t, const unsigned char** data,
    int* stride, int* w, int* h, int* spacing, int* offset)
{
    static unsigned char bits[16]{};
    *data = bits; *stride = 1; *w = 8; *h = dbc_height; *spacing = 1; *offset = 2;
    return 0;
}
uint32_t read_utf_8_codepoint_f(const char* text, size_t* length, const char*)
{
    const auto* s = reinterpret_cast<const unsigned char*>(text);
    if (*s < 128) { *length = 1; return *s; }
    assert((*s & 0xf0) == 0xe0);
    *length = 3;
    return uint32_t(s[0] & 15) * 4096 + uint32_t(s[1] & 63) * 64 + (s[2] & 63);
}
int LbTextWordWidthExplicit(const TbSpriteSheet*, const AsianFont*, TbBool dbc,
    const char* text, long units)
{
    int width = 0;
    while (*text && *text != ' ' && *text != '\n' && *text != '\t') {
        size_t n;
        const auto chr = read_utf_8_codepoint(text, &n); text += n;
        if (chr > 32) width += (dbc ? 9 : 5) * units / 16;
    }
    return width;
}
}

struct Sink final : TextLayoutSink {
    std::vector<TextLayoutGlyph> glyphs;
    int western = 0, dbc = 0;
    bool BeginWesternGlyph(const TbSprite* source) override { assert(source == &sprite); ++western; return true; }
    bool BeginDbcGlyph(const AsianFont*, uint32_t chr) override { ++dbc; return chr != '?'; }
    void Emit(const TextLayoutGlyph& glyph) override { glyphs.push_back(glyph); }
};
static IRTextDrawCmd Command()
{
    IRTextDrawCmd cmd;
    cmd.font = &font_token; cmd.pos_x = 3; cmd.pos_y = 5; cmd.units_per_px = 24;
    cmd.clip_x = cmd.justify_x = 11; cmd.clip_y = cmd.justify_y = 17;
    cmd.clip_w = cmd.justify_w = 80; cmd.clip_h = 100;
    cmd.draw_colour = 19;
    return cmd;
}
int main()
{
    auto snapshot = CaptureTextDrawState(3, 5, 24);
    assert(snapshot.font == lbFontPtr && snapshot.font_generation == 12 && !snapshot.dbc_font);
    assert(snapshot.cmap == LbTextGetRemap() && snapshot.clip_x == 11 && snapshot.clip_h == 100);
    assert(snapshot.pos_x == 3 && snapshot.pos_y == 5 && snapshot.units_per_px == 24);
    assert(snapshot.draw_flags == Lb_TEXT_HALIGN_CENTER && snapshot.draw_colour == 19);
    dbc_initialized = true;
    snapshot = CaptureTextDrawState(3, 5, 24);
    assert(snapshot.dbc_font == active_dbcfont && snapshot.dbc_colour0 == 37 && snapshot.dbc_colour1 == 41);
    auto cmd = Command();
    Sink plain; LayoutText(cmd, "AB", plain);
    assert(plain.glyphs.size() == 2 && plain.western == 2);
    assert(plain.glyphs[0].x == 14 && plain.glyphs[1].x == 21);
    assert(plain.glyphs[0].y == 22 && plain.glyphs[0].units_per_px == 24);
    cmd.draw_flags = Lb_TEXT_HALIGN_CENTER;
    Sink centered; LayoutText(cmd, "AB", centered);
    assert(centered.glyphs[0].x == 45.5f && centered.glyphs[1].x == 52.5f);

    cmd = Command(); cmd.draw_flags = Lb_TEXT_HALIGN_LEFT; cmd.justify_w = 17;
    Sink wrapped; LayoutText(cmd, "AA B", wrapped);
    assert(wrapped.glyphs.size() == 3);
    assert(wrapped.glyphs[0].x == 14 && wrapped.glyphs[1].x == 21);
    assert(wrapped.glyphs[2].x == 14 && wrapped.glyphs[2].y == 34);

    cmd = Command();
    Sink tabs; LayoutText(cmd, "A\tB\nC", tabs);
    assert(tabs.glyphs.size() == 3 && tabs.glyphs[1].x == 29);
    assert(tabs.glyphs[2].x == 14 && tabs.glyphs[2].y == 34);

    Sink controls; LayoutText(cmd, "\001A\014\xEF\x84\x87" "B", controls);
    assert(controls.glyphs.size() == 2);
    assert(controls.glyphs[0].kind == TextLayoutGlyphKind::PaletteSprite);
    assert(controls.glyphs[1].kind == TextLayoutGlyphKind::ColourSprite && controls.glyphs[1].colour == 7);
    assert(controls.glyphs[0].alpha == 1.0f / 3.0f && controls.glyphs[1].alpha == 1.0f / 3.0f);

    cmd.draw_flags = Lb_TEXT_UNDERLINE | Lb_TEXT_UNDERLNSHADOW;
    lbDisplayEx.ShadowColour = 29;
    Sink underline; LayoutText(cmd, "A", underline);
    assert(underline.glyphs.size() == 3);
    assert(underline.glyphs[1].kind == TextLayoutGlyphKind::SolidRect);
    assert(underline.glyphs[1].x == 15 && underline.glyphs[1].y == 34 && underline.glyphs[1].colour == 29);
    assert(underline.glyphs[2].x == 14 && underline.glyphs[2].y == 33 && underline.glyphs[2].colour == 19);
    assert(underline.glyphs[2].w == 7 && underline.glyphs[2].h == 1);

    cmd = Command(); cmd.dbc_enabled = 1; cmd.dbc_font = &font_token;
    cmd.dbc_colour0 = 37; cmd.dbc_colour1 = 41;
    Sink dbc16; LayoutText(cmd, "AB", dbc16);
    assert(dbc16.glyphs.size() == 4 && dbc16.dbc == 2);
    assert(dbc16.glyphs[0].x == 15 && dbc16.glyphs[0].y == 26 && dbc16.glyphs[0].colour == 41);
    assert(dbc16.glyphs[1].x == 14 && dbc16.glyphs[1].y == 25 && dbc16.glyphs[1].colour == 37);
    assert(dbc16.glyphs[3].x == 27);
    dbc_height = 12; cmd.draw_flags = Lb_TEXT_ONE_COLOR;
    Sink dbc12; LayoutText(cmd, "A?B", dbc12);
    assert(dbc12.glyphs.size() == 4 && dbc12.dbc == 3);
    assert(dbc12.glyphs[3].x == 23 && dbc12.glyphs[3].colour == 19);

    Sink missing; cmd = Command(); LayoutText(cmd, "A?B", missing);
    assert(missing.glyphs.size() == 2 && missing.glyphs[1].x == 21);
    std::puts("text layout: 10 CPU fixture groups passed");
}
