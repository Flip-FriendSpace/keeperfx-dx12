# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
"""Compile actual frontend caller code with a controlled frame-admission stub."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
def block(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
loop = block((root/'src/game_loop.c').read_text(), 'if ((!finish_menu) && (LbIsActive()))')
yield_draw = block((root/'src/main.cpp').read_text(), 'extern "C" void network_yield_draw_frontend()\n{')
join_draw = block((root/'src/front_network.c').read_text(), 'void display_attempting_to_join_message(int remaining_s)\n{')
prefix = r'''
#include <cassert>
#include <cstdio>
static int mode, presents, calls, begins, texts, key, cleared;
static bool finish_menu, attempting_to_join_cancelled;
static int frontend_menu_state;
enum { FeSt_NETLAND_VIEW=1, FeSt_TORTURE=2, FeSt_NET_START=3, KC_ESCAPE=1, KMod_DONTCARE=0, GUIStr_NetAttemptingToJoin=1 };
static int LbIsActive() { return 1; }
static short frontend_draw() { ++calls; return mode; }
static void RendererPresentFrame() { ++presents; }
static bool RendererBeginFrame() { ++begins; return true; }
static void RendererEndFrame() {}
static void draw_text_box(const char*) { ++texts; }
static int is_key_pressed(int,int) { return key; }
static void clear_key_pressed(int) { ++cleared; key=0; }
static const char* get_string(int) { return "Joining"; }
static void update_frontend_delta_time() {}
static void check_mouse_scroll() {}
static void update_velocity() {}
static void fronttorture_update() {}
static void poll_inputs() {}
static void frontnet_start_input() {}
'''
program = prefix + '\nstatic void main_loop_draw() {\n' + loop + '\n}\n' + yield_draw + '\n' + join_draw + r'''
static void run(int which) {
    if(which==0) main_loop_draw();
    else if(which==1) network_yield_draw_frontend();
    else display_attempting_to_join_message(4);
}
int main() {
    for(int which=0;which<3;++which) {
        presents=calls=begins=texts=cleared=0;key=1;attempting_to_join_cancelled=false;
        mode=2;run(which);
        assert(calls==1 && presents==0 && begins==0 && texts==0);
        if(which==2) assert(attempting_to_join_cancelled && cleared==1);
        mode=1;run(which);
        assert(calls==2 && presents==1);
        if(which==2) assert(begins==1 && texts==1);
        mode=0;run(which);
        assert(calls==3 && presents==2); // Movie return convention remains unchanged.
    }
    puts("PASS actual three callers: refused draw never presents, next draw presents, movie path and cancel retained");
}
'''
with tempfile.TemporaryDirectory(prefix='frontend-admission-') as tmp:
    p=Path(tmp); (p/'test.cpp').write_text(program)
    subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
