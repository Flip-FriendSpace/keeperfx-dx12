// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "pre_inc.h"
#include "kfx/renderer/TextLayout.h"
#include "kfx/renderer/ir/TextCommands.h"
#include "kfx/renderer/RendererManager.h"
#include "bflib_sprfnt.h"
#include "bflib_sprite.h"
#include "bflib_text.h"
#include "bflib_video.h"
#include "post_inc.h"

namespace kfx::render {
namespace {
float LineHeightExplicit(const TbSpriteSheet* font, const AsianFont* dbc_font)
{
    return dbc_font ? (float)LbDbcCharHeight(dbc_font) : (float)LbSprFontCharHeight(font, ' ');
}
struct TextDrawState { uint8_t colour; uint32_t flags; };
class TextLayout {
public:
    explicit TextLayout(TextLayoutSink& sink) : sink_(sink) {}
    void Layout(const IRTextDrawCmd& cmd, const char* text, const TbSpriteSheet* font,
        const AsianFont* dbc_font, TextDrawState& state);
private:
    TextLayoutSink& sink_;
    void FlushSegment(const char* sbuf, const char* ebuf, float x, float y,
        float space_len, int units_per_px, const TbSpriteSheet* font,
        const AsianFont* dbc_font, const IRTextDrawCmd& cmd, TextDrawState& state);
    float EmitWesternGlyph(const TbSpriteSheet* font, uint32_t chr,
        float x, float y, int units_per_px, const TextDrawState& state);
    float EmitDbcGlyph(const AsianFont* dbc_font, uint32_t chr,
        float x, float y, int units_per_px, const TextDrawState& state,
        long face_colour, long shadow_colour);
    void EmitUnderline(float x, float y, float w, float h, int units_per_px,
        const TextDrawState& state);
};
void TextLayout::Layout(const IRTextDrawCmd& cmd, const char* text, const struct TbSpriteSheet* font,
                                   const struct AsianFont* dbc_font, TextDrawState& state)
{
    const int ups = cmd.units_per_px;

    auto char_width = [&](uint32_t chr) -> float {
        return dbc_font ? (float)LbDbcCharWidthM(dbc_font, chr, ups)
                         : (float)(LbSprFontCharWidth(font, chr) * ups / 16);
    };
    auto word_width = [&](const char* s) -> float {
        return (float)LbTextWordWidthExplicit(font, dbc_font, dbc_font != nullptr, s, ups);
    };
    auto is_duospace = [&](uint32_t chr) -> bool {
        return dbc_font && LbDbcIsDuospaceChar(dbc_font, chr);
    };

    const float h = (float)((int)LineHeightExplicit(font, dbc_font) * ups / 16);
    const float justifyx = (float)(cmd.justify_x - cmd.clip_x);
    const float justifyy = (float)(cmd.justify_y - cmd.clip_y);
    float posx = (float)cmd.pos_x + justifyx;
    const float startx = posx;
    float starty = (float)cmd.pos_y + justifyy;
    const float justify_w = (float)cmd.justify_w;

    auto justified_char_pos_x = [&](float startx_l, float all_chars_width, float spr_width, float mul_width) -> float {
        const unsigned short flags = (unsigned short)state.flags;
        if (flags & Lb_TEXT_HALIGN_RIGHT)
            return startx_l + (justify_w + justifyx + mul_width * spr_width - all_chars_width);
        if (flags & Lb_TEXT_HALIGN_CENTER)
            return startx_l + (justify_w + justifyx + mul_width * spr_width - all_chars_width) / 2.0f;
        return startx_l; // LEFT, JUSTIFY (justify only adjusts width, not the anchor), and no flag set
    };
    auto justified_char_width = [&](float all_chars_width, float spr_width, long words_count) -> float {
        if (!((unsigned short)state.flags & Lb_TEXT_HALIGN_JUSTIFY))
            return spr_width;
        const float space_width = char_width(' ');
        if (words_count > 0)
            return spr_width + (justify_w + justifyx + space_width - all_chars_width) / (float)words_count;
        return spr_width;
    };

    const float clip_x = (float)cmd.clip_x;
    const float clip_y = (float)cmd.clip_y;

    long count = 0;
    const char* sbuf = text;
    const char* ebuf = text;

    while (*ebuf != '\0')
    {
        const char* seg_break = ebuf; // position just before this character
        size_t seq_len = 0;
        uint32_t chr = read_utf_8_codepoint(ebuf, &seq_len);
        if (seq_len == 0) break;
        ebuf += seq_len;

        if (chr > 32)
        {
            float w = char_width(chr);
            if (is_duospace(chr))
                count = 0;
            if ((posx + w - justifyx <= justify_w) || (count > 0) || !LbAlignMethodSet((unsigned short)state.flags))
            {
                posx += w;
                continue;
            }
            // Glyph doesn't fit and alignment is set -- emit the segment
            // built so far and wrap to the next line.
            w = char_width(' ');
            posx += w;
            float x = justified_char_pos_x(startx, posx, w, 1.0f);
            float y = (float)LbGetJustifiedCharPosY((long)starty, (long)h, (long)h, (unsigned short)state.flags);
            float len = justified_char_width(posx, w, count);
            FlushSegment(sbuf, seg_break, x + clip_x, y + clip_y, len, ups, font, dbc_font, cmd, state);
            posx = startx;
            sbuf = seg_break;
            ebuf = seg_break;
            starty += h;
            count = 0;
        }
        else if ((chr == ' ') || (chr == 0xA0))
        {
            float w = char_width(' ');
            float wordw = word_width(ebuf);
            if (posx + w + wordw - justifyx <= justify_w)
            {
                count++;
                posx += w;
                continue;
            }
            posx += w;
            float x = justified_char_pos_x(startx, posx, w, 1.0f);
            float y = (float)LbGetJustifiedCharPosY((long)starty, (long)h, (long)h, (unsigned short)state.flags);
            float len = justified_char_width(posx, w, count);
            FlushSegment(sbuf, ebuf, x + clip_x, y + clip_y, len, ups, font, dbc_font, cmd, state);
            if (LbAlignMethodSet((unsigned short)state.flags))
            {
                posx = startx;
                sbuf = ebuf;
                starty += h;
            }
            count = 0;
        }
        else if (chr == '\n')
        {
            float x = justified_char_pos_x(startx, posx, 0.0f, 1.0f);
            float len = char_width(' ');
            float y = starty;
            FlushSegment(sbuf, ebuf, x + clip_x, y + clip_y, len, ups, font, dbc_font, cmd, state);
            sbuf = ebuf;
            posx = startx;
            starty += h;
            count = 0;
        }
        else if (chr == '\t')
        {
            unsigned char spaces_per_tab = LbTextGetSpacesPerTab();
            float w = char_width(' ');
            posx += (float)spaces_per_tab * w;
            float len = word_width(ebuf);
            if (posx + len - justifyx <= justify_w)
            {
                count += spaces_per_tab;
                continue;
            }
            float x = justified_char_pos_x(startx, posx, w, (float)spaces_per_tab);
            float y = (float)LbGetJustifiedCharPosY((long)starty, (long)h, (long)h, (unsigned short)state.flags);
            len = justified_char_width(posx, w, count);
            FlushSegment(sbuf, ebuf, x + clip_x, y + clip_y, len, ups, font, dbc_font, cmd, state);
            if (LbAlignMethodSet((unsigned short)state.flags))
            {
                posx = startx;
                sbuf = ebuf;
                starty += h;
            }
            count = 0;
        }
        else if ((chr == DKChr_AlignLeft) || (chr == DKChr_AlignRight) || (chr == DKChr_AlignCenter))
        {
            if (posx - justifyx > justify_w)
            {
                float len = char_width(' ');
                FlushSegment(sbuf, ebuf, startx + clip_x, starty + clip_y, len, ups, font, dbc_font, cmd, state);
                posx = startx;
                sbuf = ebuf;
                count = 0;
                starty += h;
            }
            switch (chr)
            {
                case DKChr_AlignLeft:   state.flags ^= Lb_TEXT_HALIGN_LEFT;   break;
                case DKChr_AlignRight:  state.flags ^= Lb_TEXT_HALIGN_RIGHT;  break;
                case DKChr_AlignCenter: state.flags ^= Lb_TEXT_HALIGN_CENTER; break;
            }
        }
    }

    float x = justified_char_pos_x(startx, posx, 0.0f, 1.0f);
    float y = (float)LbGetJustifiedCharPosY((long)starty, (long)h, (long)h, (unsigned short)state.flags);
    float len = char_width(' ');
    FlushSegment(sbuf, ebuf, x + clip_x, y + clip_y, len, ups, font, dbc_font, cmd, state);
}

void TextLayout::FlushSegment(const char* sbuf, const char* ebuf, float x, float y,
                                  float space_len, int units_per_px,
                                  const struct TbSpriteSheet* font, const struct AsianFont* dbc_font,
                                  const IRTextDrawCmd& cmd, TextDrawState& state)
{
    for (const char* c = sbuf; c < ebuf; )
    {
        size_t seq_len = 0;
        uint32_t chr = read_utf_8_codepoint(c, &seq_len);
        if (seq_len == 0) break;
        c += seq_len;

        if (chr > colour_modifiers_begin && chr < colour_modifiers_end)
        {
            state.colour = (uint8_t)(chr - colour_modifiers_begin);
        }
        else if ((chr == 0xA0) || (chr == ' '))
        {
            float w = space_len;
            if (state.flags & Lb_TEXT_UNDERLINE)
                EmitUnderline(x, y, w, LineHeightExplicit(font, dbc_font) * units_per_px / 16.0f, units_per_px, state);
            x += w;
        }
        else if (chr > 32)
        {
            float w = dbc_font ? EmitDbcGlyph(dbc_font, chr, x, y, units_per_px, state, cmd.dbc_colour0, cmd.dbc_colour1)
                                : EmitWesternGlyph(font, chr, x, y, units_per_px, state);
            x += w;
        }
        else if (chr == '\t')
        {
            float w = space_len * (float)LbTextGetSpacesPerTab();
            if (state.flags & Lb_TEXT_UNDERLINE)
                EmitUnderline(x, y, w, LineHeightExplicit(font, dbc_font) * units_per_px / 16.0f, units_per_px, state);
            x += w;
        }
        else
        {
            switch (chr)
            {
            case DKChr_Modifier_Transparent4: state.flags ^= Lb_SPRITE_TRANSPAR4;  break;
            case DKChr_Modifier_Transparent8: state.flags ^= Lb_SPRITE_TRANSPAR8;  break;
            case DKChr_Modifier_Outline:      state.flags ^= Lb_SPRITE_OUTLINE;    break;
            case DKChr_Modifier_FlipHoriz:    state.flags ^= Lb_SPRITE_FLIP_HORIZ; break;
            case DKChr_Modifier_FlipVertic:   state.flags ^= Lb_SPRITE_FLIP_VERTIC;break;
            case DKChr_Modifier_Underline:    state.flags ^= Lb_TEXT_UNDERLINE;    break;
            case DKChr_Modifier_OneColor:     state.flags ^= Lb_TEXT_ONE_COLOR;    break;
            default: break; // DKChr_NewLine/Return/Colour: no-op here, matches put_down_sprites()
            }
        }
    }
}

float TextLayout::EmitWesternGlyph(const struct TbSpriteSheet* font, uint32_t chr,
                                       float x, float y, int units_per_px, const TextDrawState& state)
{
    const struct TbSprite* spr = LbFontCharSprite(font, chr);
    if (!spr || spr->SWidth == 0 || spr->SHeight == 0)
        return 0.0f;

    TextLayoutGlyph glyph;
    glyph.kind = (state.flags & Lb_TEXT_ONE_COLOR) ? TextLayoutGlyphKind::ColourSprite : TextLayoutGlyphKind::PaletteSprite;
    glyph.colour = state.colour;
    if (!sink_.BeginWesternGlyph(spr))
        return 0.0f;
    glyph.x = x;
    glyph.y = y;
    glyph.units_per_px = units_per_px;
    glyph.alpha = draw_flags_source_weight(state.flags);
    sink_.Emit(glyph);

    float w = (float)(spr->SWidth * units_per_px / 16);
    if (state.flags & Lb_TEXT_UNDERLINE)
        EmitUnderline(x, y, w, (float)LbSprFontCharHeight(font, ' ') * units_per_px / 16.0f, units_per_px, state);
    return w;
}

float TextLayout::EmitDbcGlyph(const struct AsianFont* dbc_font, uint32_t chr,
                                   float x, float y, int units_per_px, const TextDrawState& state,
                                   long face_colour, long shadow_colour)
{
    if (!sink_.BeginDbcGlyph(dbc_font, chr))
        return 0.0f;

    const unsigned char* data = nullptr;
    int scanline = 0, glyph_w = 0, glyph_h = 0, spacing = 0, voffset = 0;
    if (LbDbcGetGlyphBits(dbc_font, chr, &data, &scanline, &glyph_w, &glyph_h, &spacing, &voffset) != 0)
        return 0.0f;

    // Preserve explicit hardware DBC face/shadow colours without applying
    // the Western palette remap to these colour glyphs.
    const bool one_colour = (state.flags & Lb_TEXT_ONE_COLOR) != 0;
    const long colour_idx = one_colour ? (long)state.colour : face_colour;
    const float scale = units_per_px / 16.0f;
    const float gy = y + (float)voffset * scale;

    TextLayoutGlyph glyph;
    glyph.kind = TextLayoutGlyphKind::ColourSprite;
    glyph.units_per_px = units_per_px;
    glyph.alpha = draw_flags_source_weight(state.flags);
    // Drop shadow, always drawn
    glyph.colour = (uint8_t)shadow_colour;
    glyph.x = x + 1.0f;
    glyph.y = gy + 1.0f;
    sink_.Emit(glyph);
    glyph.colour = (uint8_t)colour_idx;
    glyph.x = x;
    glyph.y = gy;
    sink_.Emit(glyph);

    float advance = (glyph_h == 16) ? (float)((spacing + glyph_w) * units_per_px / 16) : (float)(spacing + glyph_w);
    if (state.flags & Lb_TEXT_UNDERLINE)
        EmitUnderline(x, y, advance, (float)LbDbcCharHeight(dbc_font) * scale, units_per_px, state);
    return advance;
}

void TextLayout::EmitUnderline(float x, float y, float w, float height, int units_per_px, const TextDrawState& state)
{
    if (!(state.flags & Lb_TEXT_UNDERLINE) || w <= 0.0f)
        return;

    int ups = units_per_px < 1 ? 1 : units_per_px;
    long base_height = (long)(height * 16.0f / (float)ups);
    long thickness = ((base_height > 16) ? 2 : 1) * units_per_px / 16;
    if (thickness < 1) thickness = 1;

    TextLayoutGlyph rect;
    rect.kind = TextLayoutGlyphKind::SolidRect;
    rect.alpha = draw_flags_source_weight(state.flags);
    rect.w = w;
    rect.h = 1.0f;

    float h = height;
    if (state.flags & Lb_TEXT_UNDERLNSHADOW)
    {
        long shadow_off = ((base_height > 32) ? 2 : 1) * units_per_px / 16;
        if (shadow_off < 1) shadow_off = 1;
        rect.colour = lbDisplayEx.ShadowColour;
        rect.x = x + (float)shadow_off;
        for (long i = 0; i < thickness; i++)
        {
            rect.y = y + h;
            sink_.Emit(rect);
            h -= 1.0f;
        }
    }
    rect.colour = state.colour;
    rect.x = x;
    for (long i = 0; i < thickness; i++)
    {
        rect.y = y + h;
        sink_.Emit(rect);
        h -= 1.0f;
    }
}


} // namespace

IRTextDrawCmd CaptureTextDrawState(int32_t x, int32_t y, int32_t units_per_px)
{
    IRTextDrawCmd cmd;
    cmd.pos_x = x; cmd.pos_y = y; cmd.units_per_px = units_per_px;
    cmd.draw_colour = RendererGetDrawColour();
    cmd.draw_flags = RendererGetDrawFlags();
    cmd.cmap = LbTextGetRemap();
    LbTextGetJustifyWindow(&cmd.justify_x, &cmd.justify_y, &cmd.justify_w);
    LbTextGetClipWindow(&cmd.clip_x, &cmd.clip_y, &cmd.clip_w, &cmd.clip_h);
    cmd.font = lbFontPtr;
    cmd.font_generation = LbTextGetFontGeneration();
    cmd.dbc_enabled = (dbc_initialized && dbc_enabled) ? 1 : 0;
    cmd.dbc_font = cmd.dbc_enabled ? (const void*)active_dbcfont : nullptr;
    cmd.dbc_colour0 = dbc_colour0;
    cmd.dbc_colour1 = dbc_colour1;
    return cmd;
}
void LayoutText(const IRTextDrawCmd& cmd, const char* text, TextLayoutSink& sink)
{
    const auto* font = static_cast<const TbSpriteSheet*>(cmd.font);
    const auto* dbc_font = cmd.dbc_enabled ? static_cast<const AsianFont*>(cmd.dbc_font) : nullptr;
    if (!text || (!font && !dbc_font)) return;
    TextDrawState state{cmd.draw_colour, cmd.draw_flags};
    TextLayout layout(sink);
    layout.Layout(cmd, text, font, dbc_font, state);
}
}
