// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Synchronous renderer-neutral text layout and glyph emission.
#pragma once
#include <cstdint>

struct IRTextDrawCmd;
struct TbSprite;
struct AsianFont;

namespace kfx::render {
enum class TextLayoutGlyphKind : uint8_t { PaletteSprite, ColourSprite, SolidRect };

// Scalar output only. A sprite glyph uses the source most recently accepted by
// BeginWesternGlyph/BeginDbcGlyph; a SolidRect requires no source.
struct TextLayoutGlyph {
    TextLayoutGlyphKind kind = TextLayoutGlyphKind::PaletteSprite;
    uint8_t colour = 0;
    float x = 0, y = 0, w = 0, h = 0;
    int32_t units_per_px = 16;
    float alpha = 1;
};

class TextLayoutSink {
public:
    virtual ~TextLayoutSink() = default;
    // Producer-thread only: resolve/copy synchronously; no font/sprite pointers
    // may be retained in an owned frame. False means a zero-advance glyph.
    virtual bool BeginWesternGlyph(const TbSprite* sprite) = 0;
    virtual bool BeginDbcGlyph(const AsianFont* font, uint32_t codepoint) = 0;
    virtual void Emit(const TextLayoutGlyph& glyph) = 0;
};

// Same ambient state as deferred text, without appending an IR command. Font
// and remap pointers are borrowed for immediate layout only.
IRTextDrawCmd CaptureTextDrawState(int32_t x, int32_t y, int32_t units_per_px);

// Preserves hardware wrapping, fractional positions, advances, control codes,
// DBC face/shadow passes and underline order. The sink applies cmd's clip and
// palette remap; colour glyphs use their explicit colour. Draws no pixels and
// retains neither string, fonts, remap nor sink.
void LayoutText(const IRTextDrawCmd& cmd, const char* text, TextLayoutSink& sink);
}
