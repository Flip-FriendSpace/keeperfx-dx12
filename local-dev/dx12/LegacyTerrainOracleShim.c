// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Compile the actual original source, with isolated external symbol names.
// Its algorithms and original license/copyright notices remain unmodified.
// This translation unit needs no game, display, sprite or simulation state.
#define draw_gpoly LegacyTerrainDrawGpoly
#define vec_mode LegacyTerrainVecMode
#define vec_screen LegacyTerrainVecScreen
#define vec_map LegacyTerrainVecMap
#define vec_screen_width LegacyTerrainVecScreenWidth
#define vec_window_width LegacyTerrainVecWindowWidth
#define vec_window_height LegacyTerrainVecWindowHeight
#define render_fade_tables LegacyTerrainFadeTables
#define get_gameturn LegacyTerrainUnexpectedGameTurn
#define LbErrorLog LegacyTerrainUnexpectedErrorLog
#define WorldSceneTraceTriangle LegacyTerrainIgnoreCapture
#include "../../src/kfx/renderer/software/bflib_render_gpoly.c"
#include "LegacyTerrainOracleShim.h"

unsigned char vec_mode;
unsigned char* vec_screen;
unsigned char* vec_map;
unsigned long vec_screen_width;
long vec_window_width;
long vec_window_height;
unsigned char* render_fade_tables;

GameTurn get_gameturn(void) { abort(); }
int LbErrorLog(const char* format, ...)
{
    (void)format;
    abort();
}
// The producer's diagnostic hook is deliberately inert in this standalone
// reference; it must never inspect or publish live scene/simulation state.
void WorldSceneTraceTriangle(unsigned rasterizer, const struct PolyPoint* a,
    const struct PolyPoint* b, const struct PolyPoint* c, unsigned mode,
    unsigned colour, const unsigned char* texture)
{
    (void)rasterizer; (void)a; (void)b; (void)c;
    (void)mode; (void)colour; (void)texture;
}

void LegacyTerrainOracleDraw(const struct LegacyTerrainPoint points[3],
    uint8_t* pixels, uint32_t width, uint32_t height,
    const uint8_t* texture, const uint8_t* fade)
{
    struct PolyPoint original[3];
    for (int i = 0; i < 3; ++i) {
        original[i].X = points[i].x;
        original[i].Y = points[i].y;
        original[i].U = points[i].u;
        original[i].V = points[i].v;
        original[i].S = points[i].s;
    }
    vec_mode = VM_QuadTextured;
    vec_screen = pixels;
    vec_screen_width = width;
    vec_window_width = width;
    vec_window_height = height;
    vec_map = (unsigned char*)texture;
    render_fade_tables = (unsigned char*)fade;
    draw_gpoly(&original[0], &original[1], &original[2]);
}
void LegacyTerrainOracleClear(void)
{
    vec_screen = vec_map = render_fade_tables = NULL;
    vec_screen_width = 0;
    vec_window_width = vec_window_height = 0;
    vec_mode = 0;
}
