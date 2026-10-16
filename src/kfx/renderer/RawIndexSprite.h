// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX remaster changes: 2026-10-16.
/* Exact indexed RGBA input for source-code sprites. RGBA channels are bytes,
 * R=G=B is the palette code, A>=64 is coverage. No colour quantization. */
#ifndef KFX_RAW_INDEX_SPRITE_H
#define KFX_RAW_INDEX_SPRITE_H
#include <stddef.h>
#include <stdint.h>

static size_t kfx_raw_index_rle(uint8_t *out, size_t capacity,
    const uint8_t *rgba, size_t width, size_t height)
{
    if (!out || !rgba || !width || !height || width > 1024 || height > 1024)
        return 0;
    const size_t required = (width * 2 + 1) * height;
    if (capacity < required)
        return 0;
    /* Validate before writing anything, including transparent/opaque boundaries. */
    for (size_t i = 0; i < width * height; ++i)
    {
        const uint8_t *p = rgba + i * 4;
        if (p[3] >= 64 && (p[0] != p[1] || p[0] != p[2]))
            return 0;
    }
    size_t used = 0;
    for (size_t y = 0; y < height; ++y)
    {
        size_t x = 0;
        while (x < width)
        {
            const uint8_t *p = rgba + (y * width + x) * 4;
            const int covered = p[3] >= 64;
            size_t run = 1;
            while (x + run < width && run < 127
                && (rgba[(y * width + x + run) * 4 + 3] >= 64) == covered)
                ++run;
            out[used++] = covered ? (uint8_t)run : (uint8_t)(256 - run);
            if (covered)
                for (size_t i = 0; i < run; ++i)
                    out[used++] = rgba[(y * width + x + i) * 4];
            x += run;
        }
        out[used++] = 0;
    }
    return used;
}
#endif
