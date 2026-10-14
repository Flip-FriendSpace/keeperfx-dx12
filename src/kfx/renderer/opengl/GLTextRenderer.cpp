// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "pre_inc.h"
#include "kfx/renderer/opengl/GLTextRenderer.h"
#include "kfx/renderer/TextLayout.h"
#include "kfx/renderer/opengl/GLUIRenderer.h"
#include "kfx/renderer/opengl/GLFunctions.h" // glScissor/GL_SCISSOR_TEST
#include "kfx/renderer/ir/TextCommands.h"
#include "kfx/renderer/RendererManager.h"
#include "bflib_sprfnt.h"
#include "bflib_sprite.h"
#include "bflib_text.h" // read_utf_8_codepoint
#include "bflib_video.h" // Lb_TEXT_*/Lb_SPRITE_* flags, lbDisplayEx
#include "post_inc.h"

namespace {
class GLTextLayoutSink final : public kfx::render::TextLayoutSink {
public:
    GLTextLayoutSink(GLUIRenderer& ui, TextCommandBuffers& out) : ui_(ui), out_(out) {}
    bool BeginWesternGlyph(const TbSprite* sprite) override
    {
        sprite_ = ui_.ResolveSprite(sprite);
        // Retain Western advance even when atlas resolution fails.
        return true;
    }
    bool BeginDbcGlyph(const AsianFont* font, uint32_t codepoint) override
    {
        sprite_ = ui_.ResolveDbcGlyph(font, codepoint);
        return sprite_ != kInvalidSpriteHandle;
    }
    void Emit(const kfx::render::TextLayoutGlyph& source) override
    {
        IRTextGlyph glyph;
        switch (source.kind) {
        case kfx::render::TextLayoutGlyphKind::PaletteSprite:
            glyph.kind = IRTextGlyphKind::PaletteSprite; break;
        case kfx::render::TextLayoutGlyphKind::ColourSprite:
            glyph.kind = IRTextGlyphKind::ColourSprite; break;
        case kfx::render::TextLayoutGlyphKind::SolidRect:
            glyph.kind = IRTextGlyphKind::SolidRect; break;
        }
        glyph.sprite = source.kind == kfx::render::TextLayoutGlyphKind::SolidRect
            ? kInvalidSpriteHandle : sprite_;
        glyph.colour = source.colour;
        glyph.x = source.x; glyph.y = source.y;
        glyph.w = source.w; glyph.h = source.h;
        glyph.units_per_px = source.units_per_px;
        glyph.alpha = source.alpha;
        out_.glyphs.Append(glyph);
    }
private:
    GLUIRenderer& ui_;
    TextCommandBuffers& out_;
    SpriteHandle sprite_ = kInvalidSpriteHandle;
};
} // namespace

TbBool GLTextRenderer::DrawTextResized(int32_t x, int32_t y, int32_t units_per_px, const char* text)
{
    IRTextDrawCmd* cmd = AppendTextCommand(x, y, units_per_px);
    if (cmd == nullptr)
        return LbTextDrawResizedImmediate(x, y, units_per_px, text);

    cmd->glyph_first = (uint32_t)m_text_write_cmds->glyphs.Size();
    if (m_ui)
    {
        GLTextLayoutSink sink(*m_ui, *m_text_write_cmds);
        kfx::render::LayoutText(*cmd, text, sink);
    }
    cmd->glyph_count = (uint32_t)m_text_write_cmds->glyphs.Size() - cmd->glyph_first;
    return true;
}

void GLTextRenderer::DrawGlyphs(const IRTextDrawCmd& cmd, const TextCommandBuffers& text)
{
    if (!m_ui || cmd.glyph_count == 0)
        return;
    if ((size_t)cmd.glyph_first + cmd.glyph_count > text.glyphs.Size())
        return;

    const int screen_h = m_ui->GetScreenHeight();
    const int sx = cmd.clip_x;
    const int sy = screen_h - (cmd.clip_y + cmd.clip_h); // top-left origin -> GL's bottom-left
    const int sw = cmd.clip_w;
    const int sh = cmd.clip_h;
    const bool scissor_valid = (sw > 0) && (sh > 0);
    if (scissor_valid)
    {
        glEnable(GL_SCISSOR_TEST);
        glScissor(sx, sy, sw, sh);
    }

    const IRTextGlyph* glyphs = text.glyphs.Data() + cmd.glyph_first;
    for (uint32_t i = 0; i < cmd.glyph_count; ++i)
    {
        const IRTextGlyph& glyph = glyphs[i];
        float r = 1.0f, g = 1.0f, b = 1.0f;
        if (glyph.kind != IRTextGlyphKind::PaletteSprite)
            m_ui->PaletteColour(glyph.colour, &r, &g, &b);
        switch (glyph.kind)
        {
        case IRTextGlyphKind::PaletteSprite:
            m_ui->DrawGlyphQuad(glyph.sprite, glyph.x, glyph.y, glyph.units_per_px, 1.0f, 1.0f, 1.0f, glyph.alpha, /*sample_palette=*/true, cmd.cmap);
            break;
        case IRTextGlyphKind::ColourSprite:
            m_ui->DrawGlyphQuad(glyph.sprite, glyph.x, glyph.y, glyph.units_per_px, r, g, b, glyph.alpha, /*sample_palette=*/false);
            break;
        case IRTextGlyphKind::SolidRect:
            m_ui->DrawSolidRect(glyph.x, glyph.y, glyph.w, glyph.h, r, g, b, glyph.alpha);
            break;
        }
    }

    if (scissor_valid)
        glDisable(GL_SCISSOR_TEST);
}
