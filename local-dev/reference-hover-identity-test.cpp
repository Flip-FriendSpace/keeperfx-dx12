// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/WorldScene.h"
#include <cassert>
#include <iostream>

int main()
{
    kfx::render::SceneIncarnations ids(8);
    assert(ids.Peek(0).incarnation == 0);
    assert(ids.Peek(1).incarnation == 0);
    assert(ids.Peek(8).incarnation == 0);
    ids.Allocate(1);
    const auto first = ids.Peek(1);
    assert(first.slot == 1 && first.incarnation != 0);
    assert(ids.Peek(1).incarnation == first.incarnation);
    ids.Allocate(1); // No simulation tick is involved in allocation identity.
    const auto reused = ids.Peek(1);
    assert(reused.incarnation > first.incarnation);
    ids.Reset();
    assert(ids.Peek(1).incarnation == 0);
    ids.Allocate(1);
    assert(ids.Peek(1).incarnation > reused.incarnation);
    std::cout << "8 read-only identity/reuse/reset controls passed\n";
}
