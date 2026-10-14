// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Link with src/bflib_keybrd.c: exercise the actual event handler used by gameplay.
#include "bflib_keybrd.h"
#include <cassert>
#include <cstdio>
#include <cstring>

extern "C" void init_inputcontrol(void) {}

static void reset_input()
{
    std::memset(lbKeyOn, 0, sizeof(lbKeyOn));
    std::memset(lbKeyPressed, 0, sizeof(lbKeyPressed));
    lbInkey = KC_UNASSIGNED;
    lbBufferKeyPresses = false;
}

int main()
{
    reset_input();
    keyboardControl(KActn_KEYDOWN, KC_SPACE, KMod_NONE, ' ');
    keyboardControl(KActn_KEYUP, KC_SPACE, KMod_NONE, ' ');
    assert(lbKeyOn[KC_SPACE] == 0 && lbKeyPressed[KC_SPACE] == 0);

    reset_input();
    lbBufferKeyPresses = true;
    keyboardControl(KActn_KEYDOWN, KC_SPACE, KMod_NONE, ' ');
    keyboardControl(KActn_KEYUP, KC_SPACE, KMod_NONE, ' ');
    lbBufferKeyPresses = false;
    assert(lbKeyOn[KC_SPACE] == 0 && lbKeyPressed[KC_SPACE] == 1);
    // The logic input-pass boundary clears pulses, not held-key state.
    std::memset(lbKeyPressed, 0, sizeof(lbKeyPressed));
    assert(lbKeyOn[KC_SPACE] == 0 && lbKeyPressed[KC_SPACE] == 0);

    reset_input();
    lbBufferKeyPresses = true;
    keyboardControl(KActn_KEYDOWN, KC_LEFT, KMod_NONE, 0);
    std::memset(lbKeyPressed, 0, sizeof(lbKeyPressed));
    assert(lbKeyOn[KC_LEFT] == 1 && lbKeyPressed[KC_LEFT] == 0);
    keyboardControl(KActn_KEYUP, KC_LEFT, KMod_NONE, 0);
    assert(lbKeyOn[KC_LEFT] == 0 && lbKeyPressed[KC_LEFT] == 0);

    reset_input();
    lbBufferKeyPresses = true;
    keyboardControl(KActn_KEYDOWN, KC_A, KMod_CONTROL, 'a');
    keyboardControl(KActn_KEYUP, KC_A, KMod_NONE, 'a');
    assert(lbKeyOn[KC_A] == 0 && lbKeyOn[KC_LCONTROL] == 0);
    assert(lbKeyPressed[KC_A] == (1 | KMod_CONTROL));

    reset_input();
    lbBufferKeyPresses = true;
    keyboardControl(KActn_KEYDOWN, KC_SPACE, KMod_NONE, ' ');
    keyboardControl(KActn_KEYUP, KC_SPACE, KMod_NONE, ' ');
    keyboardControl(KActn_KEYDOWN, KC_ESCAPE, KMod_NONE, 0);
    keyboardControl(KActn_KEYUP, KC_ESCAPE, KMod_NONE, 0);
    assert(lbKeyPressed[KC_SPACE] == 1 && lbKeyPressed[KC_ESCAPE] == 1);
    std::memset(lbKeyPressed, 0, sizeof(lbKeyPressed));
    for (unsigned int i = 0; i < KC_LIST_END; ++i)
        assert(lbKeyPressed[i] == 0 && lbKeyOn[i] == 0);

    reset_input();
    lbBufferKeyPresses = true;
    keyboardControl(KActn_KEYDOWN, KC_BACK, KMod_NONE, 0);
    keyboardControl(KActn_KEYDOWN, KC_BACK, KMod_NONE, 0);
    assert(lbKeyPressed[KC_BACK] == 1);
    std::memset(lbKeyPressed, 0, sizeof(lbKeyPressed));
    keyboardControl(KActn_KEYUP, KC_BACK, KMod_NONE, 0);
    assert(lbKeyPressed[KC_BACK] == 0 && lbKeyOn[KC_BACK] == 0);

    std::puts("keyboard-latch: 6 event groups passed");
    return 0;
}
