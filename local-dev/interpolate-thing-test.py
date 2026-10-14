# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
"""Compile actual old/new interpolate_thing bodies with focused presentation stubs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
baseline = root.parent / "artifacts/dx12/temporal-state-20261013/engine_render-before.c"
def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]
old = function(baseline.read_text(), "struct ThingInterpolateResult interpolate_thing(")
source = (root / "src/engine_render.c").read_text()
new = function(source, "struct ThingInterpolateResult interpolate_thing(")
helpers = function(source, "float interpolate(") + "\n" + function(source, "float interpolate_synced(")
lerp = function((root / "src/bflib_math.c").read_text(), "float LbLerp(")
program = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct CoordPart { int32_t val; };
struct Coord3d { struct CoordPart x,y,z; };
struct Thing { uint8_t untouched[29]; uint32_t creation_turn; struct Coord3d mappos,previous_mappos;
    unsigned short floor_height,previous_floor_height; uint8_t tail[31]; };
struct ThingInterpolateResult { struct Coord3d mappos; int32_t floor_height; };
static uint32_t turn;
static unsigned feature, logs;
static float interpolate_time;
static struct { unsigned operation_flags; } game;
#define Ft_DeltaTime 1
#define GOF_Paused 1
#define flag_is_set(value,flag) ((value)&(flag))
#define is_feature_on(flag) (feature)
#define ERRORLOG(...) (++logs)
static uint32_t get_gameturn(void) { return turn; }
""" + lerp + "\n" + helpers + "\n" + old.replace("interpolate_thing(", "original_interpolate_thing(", 1) + "\n" + new + r"""
int main(void)
{
    const int distances[]={0,1,-1,9999,10000,10001,-9999,-10000,-10001};
    const float times[]={0,0.125f,0.5f,0.875f,1};
    const uint32_t ages[]={0,1,2,17,UINT32_MAX};
    unsigned checks=0,old_writes=0;
    for(unsigned age=0;age<5;++age)for(unsigned pause=0;pause<2;++pause)
    for(unsigned enabled=0;enabled<2;++enabled)for(unsigned t=0;t<5;++t)
    for(unsigned axis=0;axis<3;++axis)for(unsigned delta=0;delta<9;++delta)for(unsigned large=0;large<2;++large){
        struct Thing input; memset(&input,0xa5,sizeof(input));
        turn=100;input.creation_turn=turn-ages[age];game.operation_flags=pause;feature=enabled;interpolate_time=times[t];
        input.mappos=(struct Coord3d){{large?16777217:10371},{21003},{-173}};
        input.previous_mappos=input.mappos;
        if(axis==0)input.previous_mappos.x.val+=distances[delta];
        if(axis==1)input.previous_mappos.y.val+=distances[delta];
        if(axis==2)input.previous_mappos.z.val+=distances[delta];
        input.floor_height=127;input.previous_floor_height=1023;
        struct Thing old_input=input,new_input=input;
        logs=0;const struct ThingInterpolateResult a=original_interpolate_thing(&old_input);unsigned old_logs=logs;
        logs=0;const struct ThingInterpolateResult b=interpolate_thing(&new_input);
        assert(a.mappos.x.val==b.mappos.x.val&&a.mappos.y.val==b.mappos.y.val&&a.mappos.z.val==b.mappos.z.val&&a.floor_height==b.floor_height);
        assert(logs==old_logs&&memcmp(&input,&new_input,sizeof(input))==0);
        if(memcmp(&input,&old_input,sizeof(input)))++old_writes;
        ++checks;
    }
    assert(old_writes>0);
    printf("PASS %u actual-function pose/log comparisons; input bytes unchanged; %u original writes observed\n",checks,old_writes);
}
"""
with tempfile.TemporaryDirectory(prefix="interpolate-thing-") as temporary:
    path = Path(temporary)
    (path / "test.c").write_text(program)
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
