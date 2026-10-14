// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Compile the actual original Trig implementation with isolated symbols.
// Its algorithms and source license/copyright notices remain unmodified.
#define trig LegacyTerrainTrig_trig
#define trig_reorder_input_points LegacyTerrainTrig_trig_reorder_input_points
#define trig_ll_start LegacyTerrainTrig_trig_ll_start
#define trig_rl_start LegacyTerrainTrig_trig_rl_start
#define trig_fb_start LegacyTerrainTrig_trig_fb_start
#define trig_ft_start LegacyTerrainTrig_trig_ft_start
#define trig_render_md00 LegacyTerrainTrig_trig_render_md00
#define trig_render_md01 LegacyTerrainTrig_trig_render_md01
#define trig_render_md02 LegacyTerrainTrig_trig_render_md02
#define trig_render_md03 LegacyTerrainTrig_trig_render_md03
#define trig_render_md04 LegacyTerrainTrig_trig_render_md04
#define trig_render_md05 LegacyTerrainTrig_trig_render_md05
#define trig_render_md06 LegacyTerrainTrig_trig_render_md06
#define trig_render_md07 LegacyTerrainTrig_trig_render_md07
#define trig_render_md08 LegacyTerrainTrig_trig_render_md08
#define trig_render_md09 LegacyTerrainTrig_trig_render_md09
#define trig_render_md10 LegacyTerrainTrig_trig_render_md10
#define trig_render_md12 LegacyTerrainTrig_trig_render_md12
#define trig_render_md13 LegacyTerrainTrig_trig_render_md13
#define trig_render_md14 LegacyTerrainTrig_trig_render_md14
#define trig_render_md15 LegacyTerrainTrig_trig_render_md15
#define trig_render_md16 LegacyTerrainTrig_trig_render_md16
#define trig_render_md17 LegacyTerrainTrig_trig_render_md17
#define trig_render_md18 LegacyTerrainTrig_trig_render_md18
#define trig_render_md19 LegacyTerrainTrig_trig_render_md19
#define trig_render_md20 LegacyTerrainTrig_trig_render_md20
#define trig_render_md21 LegacyTerrainTrig_trig_render_md21
#define trig_render_md22 LegacyTerrainTrig_trig_render_md22
#define trig_render_md23 LegacyTerrainTrig_trig_render_md23
#define trig_render_md24 LegacyTerrainTrig_trig_render_md24
#define trig_render_md25 LegacyTerrainTrig_trig_render_md25
#define trig_render_md26 LegacyTerrainTrig_trig_render_md26
#define vec_mode LegacyTerrainTrig_vec_mode
#define vec_colour LegacyTerrainTrig_vec_colour
#define vec_map LegacyTerrainTrig_vec_map
#define poly_screen LegacyTerrainTrig_poly_screen
#define vec_screen_width LegacyTerrainTrig_vec_screen_width
#define vec_window_width LegacyTerrainTrig_vec_window_width
#define vec_window_height LegacyTerrainTrig_vec_window_height
#define polyscans LegacyTerrainTrig_polyscans
#define pixmap LegacyTerrainTrig_pixmap
#define render_ghost LegacyTerrainTrig_render_ghost
#define render_alpha LegacyTerrainTrig_render_alpha
#define get_gameturn LegacyTerrainTrig_get_gameturn
#define LbErrorLog LegacyTerrainTrig_LbErrorLog
#define WorldSceneTraceTriangle LegacyTerrainTrig_WorldSceneTraceTriangle
#include "../../src/kfx/renderer/software/bflib_render_trig.c"
#include "LegacyTerrainOracleShim.h"
#include <stdatomic.h>

unsigned char vec_mode;
TbPixel vec_colour;
unsigned char* vec_map;
unsigned char* poly_screen;
unsigned long vec_screen_width;
long vec_window_width;
long vec_window_height;
struct PolyPoint* polyscans;
struct TbColorTables pixmap;
unsigned char* render_ghost;
unsigned char* render_alpha;
static atomic_flag oracle_active = ATOMIC_FLAG_INIT;

static void ClearTarget(void)
{
    vec_mode = vec_colour = 0;
    vec_map = poly_screen = NULL; polyscans = NULL;
    vec_screen_width = 0; vec_window_width = vec_window_height = 0;
}
static int EndDraw(int status)
{
    ClearTarget();
    atomic_flag_clear(&oracle_active);
    return status;
}

GameTurn get_gameturn(void) { abort(); }
int LbErrorLog(const char* format, ...) { (void)format; abort(); }
void WorldSceneTraceTriangle(unsigned rasterizer, const struct PolyPoint* a,
    const struct PolyPoint* b, const struct PolyPoint* c, unsigned mode,
    unsigned colour, const unsigned char* texture)
{
    (void)rasterizer; (void)a; (void)b; (void)c;
    (void)mode; (void)colour; (void)texture;
}

static int DrawTrig(const struct LegacyTerrainPoint points[3],
    uint8_t* pixels, uint32_t width, uint32_t height,
    const uint8_t* texture, const uint8_t* fade, uint8_t colour, unsigned mode)
{
    // All original preparation paths clip the scan count to window height.
    // Guard its independent scan buffer as well as the caller's framebuffer.
    static struct PolyPoint scans[4096 + 2];
    struct PolyPoint original[3];
    unsigned char guard[sizeof(struct PolyPoint)];
    if (!width || !height || width > 4096 || height > 4096 || colour >= 64
        || !points || !pixels || !texture || !fade || (mode != 2 && mode != 7 && mode != 10)) return -1;
    if (atomic_flag_test_and_set(&oracle_active)) return -3;
    memset(guard, 0xa7, sizeof(guard));
    memset(scans, 0, sizeof(scans));
    memcpy(&scans[0], guard, sizeof(guard));
    memcpy(&scans[height + 1], guard, sizeof(guard));
    for (int i = 0; i < 3; ++i) {
        original[i].X = points[i].x; original[i].Y = points[i].y;
        original[i].U = points[i].u; original[i].V = points[i].v;
        original[i].S = 0; // Modes 2, 7 and 10 do not consume source S.
    }
    // LL/RL mode-02 preparation leaves u_step/v_step uninitialized when
    // its rounded horizontal extent is zero. Refuse that undefined input;
    // do not initialize or patch the original kernel and call it parity.
    struct PolyPoint *a = &original[0], *b = &original[1], *c = &original[2];
    const unsigned start = trig_reorder_input_points(&a, &b, &c);
    if (start == RendStart_LL) {
        const long ratio = ((b->Y - a->Y) << 16) / (c->Y - a->Y);
        const long extent = (ratio * (a->X - c->X) >> 16) + b->X - a->X;
        if (extent == 0) return EndDraw(-2);
    } else if (start == RendStart_RL) {
        const long ratio = ((c->Y - a->Y) << 16) / (b->Y - a->Y);
        const long extent = (ratio * (b->X - a->X) >> 16) + a->X - c->X;
        if (extent == 0) return EndDraw(-2);
    }
    vec_mode = (unsigned char)mode; vec_colour = colour;
    vec_map = (unsigned char*)texture;
    // Original render loops advance to the first row before writing.
    poly_screen = pixels - width;
    vec_screen_width = width; vec_window_width = width; vec_window_height = height;
    polyscans = scans + 1;
    if (mode != 2) memcpy(pixmap.fade_tables, fade, sizeof(pixmap.fade_tables));
    trig(&original[0], &original[1], &original[2]);
    return EndDraw(memcmp(&scans[0], guard, sizeof(guard)) == 0
        && memcmp(&scans[height + 1], guard, sizeof(guard)) == 0 ? 0 : -1);
}
int LegacyTerrainOracleDrawTrig(const struct LegacyTerrainPoint points[3],
    uint8_t* pixels, uint32_t width, uint32_t height,
    const uint8_t* texture, const uint8_t* fade, uint8_t colour)
{
    return DrawTrig(points, pixels, width, height, texture, fade, colour, 7);
}
int LegacyShadowOracleDrawTrig(const struct LegacyTerrainPoint points[3],
    uint8_t* pixels, uint32_t width, uint32_t height,
    const uint8_t* texture, const uint8_t* fade, uint8_t shade, int probe)
{
    return DrawTrig(points, pixels, width, height, texture, fade, shade, probe ? 2 : 10);
}
void LegacyTerrainOracleClearTrig(void)
{
    // Targets are cleared before releasing every draw. A separate caller's
    // cleanup must not invalidate an in-flight shadow/terrain invocation.
    if (!atomic_flag_test_and_set(&oracle_active)) {
        ClearTarget(); atomic_flag_clear(&oracle_active);
    }
}
