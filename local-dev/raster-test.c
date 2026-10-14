// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "pre_inc.h"
#include "bflib_render.h"
#include "bflib_vidraw.h"
#include "globals.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned char *render_fade_tables, *vec_map, *vec_screen;
unsigned char vec_mode = VM_QuadTextured;
unsigned long vec_screen_width = 96;
long vec_window_width = 80, vec_window_height = 64;
GameTurn get_gameturn(void) { return 0; }
int LbErrorLog(const char *format, ...) { (void)format; abort(); }

int main(void)
{
    unsigned char framebuffer[96 * 66], texture[8192], fades[65536];
    memset(texture, 17, sizeof(texture));
    memset(fades, 37, sizeof(fades));
    render_fade_tables = fades;
    vec_map = texture;
    vec_screen = framebuffer + 96;
    const int cases[][6] = {
        {20, -20, 60, 20, 25, 55},  // whole-width path, top clipped
        {-20, -20, 60, 20, 25, 55}, // X and Y clipped
        {20, -40, 60, -10, 25, 55}, // both top vertices above screen
        {-20, -40, 60, -10, 25, 85},
        {20, 10, 100, 30, 25, 85},  // right and bottom clipped
        {20, 10, 60, 20, 25, 55},   // fully visible
    };
    uint32_t hash = 2166136261u;
    for (size_t n = 0; n < sizeof(cases) / sizeof(cases[0]); ++n) {
        memset(framebuffer, 0xa5, sizeof(framebuffer));
        struct PolyPoint p[3] = {{0}};
        for (int i = 0; i < 3; ++i) { p[i].X = cases[n][i * 2]; p[i].Y = cases[n][i * 2 + 1]; }
        draw_gpoly(&p[0], &p[1], &p[2]);
        unsigned changed = 0;
        for (size_t i = 0; i < sizeof(framebuffer); ++i) {
            const int row = (int)(i / 96) - 1;
            const int column = i % 96;
            if (row < 0 || row >= 64 || column >= 80) assert(framebuffer[i] == 0xa5);
            else if (framebuffer[i] == 37) ++changed;
            hash = (hash ^ framebuffer[i]) * 16777619u;
        }
        assert(changed > 0);
    }
    printf("%08x\n", (unsigned)hash);
    return 0;
}
