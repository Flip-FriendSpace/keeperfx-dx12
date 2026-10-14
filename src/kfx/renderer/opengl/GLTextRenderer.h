// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#ifndef RENDERER_OPENGL_GLTEXTRENDERER_H
#define RENDERER_OPENGL_GLTEXTRENDERER_H

#include "kfx/renderer/ITextRenderer.h"
#include <cstdint>

class GLUIRenderer;
struct IRTextDrawCmd;
struct TextCommandBuffers;
struct TbSpriteSheet;
struct AsianFont;

class GLTextRenderer : public ITextRenderer {
public:
    void SetUIRenderer(GLUIRenderer* ui) { m_ui = ui; }

    /** Game thread: captures the command and lays out its glyphs. */
    TbBool DrawTextResized(int32_t x, int32_t y, int32_t units_per_px, const char* text) override;

    /** Render thread: draws the glyphs laid out for cmd at submission. */
    void DrawGlyphs(const IRTextDrawCmd& cmd, const TextCommandBuffers& text);

    const char* GetName() const override { return "GL-TEXT"; }

private:
    GLUIRenderer* m_ui = nullptr;

};

#endif // RENDERER_OPENGL_GLTEXTRENDERER_H
