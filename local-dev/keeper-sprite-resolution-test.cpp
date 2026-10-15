// SPDX-License-Identifier: GPL-2.0-or-later
#include "kfx/renderer/KeeperSpriteResolution.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>

int main()
{
    unsigned checks = 0;
    // Texture resolution must never change projected size, mirrored anchors,
    // shadow dimensions or the original path, including fractional/low zoom.
    for (unsigned shift = 0; shift <= 3; ++shift) {
        const int resolution = 1 << shift;
        const unsigned flags = (shift << 8) | 1; // NoShadows is independent.
        assert(keeper_sprite_resolution_shift(flags) == shift);
        for (long pixel = -255; pixel <= 255; ++pixel) {
            assert(keeper_sprite_logical_pixels(pixel * resolution, flags) == pixel);
            for (long scale : {1L, 3L, 7L, 15L, 31L, 32L, 33L, 63L, 127L, 255L, 1023L}) {
                const long expected = (long)std::floor(double(pixel) * scale / 32.0);
                assert(keeper_sprite_scale_pixels(pixel * resolution, scale, flags) == expected);
                ++checks;
            }
        }
    }
    // Scaling before the division matters at the smallest supported zoom.
    assert(keeper_sprite_scale_pixels(188, 3, 2 << 8) == 4);
    assert(keeper_sprite_scale_pixels(-129, 1, 2 << 8) == -2);
    assert(keeper_sprite_resolution_shift(1) == 0);
    std::printf("Keeper sprite resolution: %u projection checks passed\n", checks);
}
