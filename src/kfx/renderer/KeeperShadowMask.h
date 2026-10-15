// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KFX_KEEPER_SHADOW_MASK_H
#define KFX_KEEPER_SHADOW_MASK_H
#include <stddef.h>
#include <string.h>

/* The legacy software triangle sampler addresses a 256x256 mask. Large custom
 * sprites retain full colour detail; only this fallback coverage is reduced. */
static inline int keeper_shadow_logical_mask(unsigned char *target,
    const unsigned char *source, int width, int height, int stride, int factor)
{
    if (!target || !source || width <= 0 || height <= 0 || width > stride
        || width > 512 || height > 512 || factor < 2 || factor > 8
        || (width + factor - 1) / factor > 256 || (height + factor - 1) / factor > 256)
        return 0;
    memset(target, 0, 256 * 256);
    for (int y = 0; y < (height + factor - 1) / factor; ++y)
        for (int x = 0; x < (width + factor - 1) / factor; ++x)
            target[y * 256 + x] = source[y * factor * stride + x * factor];
    return 1;
}
#endif
