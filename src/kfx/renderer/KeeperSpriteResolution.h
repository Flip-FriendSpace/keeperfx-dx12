// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KFX_KEEPER_SPRITE_RESOLUTION_H
#define KFX_KEEPER_SPRITE_RESOLUTION_H

#include <stdint.h>

/* Runtime/captured frame flags reserve bits 8-9 for log2(texture resolution).
 * Keeping this in the existing flags preserves the disk sprite and save layouts.
 * Public sprite JSON uses resolution_scale, not these internal flag bits. */
#define KEEPER_SPRITE_RESOLUTION_MASK 0x0300

static inline unsigned keeper_sprite_resolution_shift(unsigned flags)
{
    return (flags & KEEPER_SPRITE_RESOLUTION_MASK) >> 8;
}

static inline long keeper_sprite_scale_pixels(long pixels, long scale, unsigned flags)
{
    /* Divide after multiplication: dividing scale first loses detail at low zoom.
     * Arithmetic right shift retains the legacy floor rounding of negative anchors. */
    return (long)(((int64_t)pixels * scale) >> (5 + keeper_sprite_resolution_shift(flags)));
}

static inline long keeper_sprite_logical_pixels(long pixels, unsigned flags)
{
    return pixels >> keeper_sprite_resolution_shift(flags);
}

#endif
