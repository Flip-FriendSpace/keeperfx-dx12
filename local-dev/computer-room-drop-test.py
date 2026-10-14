# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
"""Exercise the actual AI room-task body with controlled job/drop dependencies.

The pre-fix variant removes only the repair and uses GCC's deterministic automatic
variable pattern initialization. This demonstrates the invalid input without
relying on accidental stack contents; it is not an assertion that all observed
training-drop failures followed this branch.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "src/player_comptask.c").read_text()
start = source.index("long task_move_creature_to_room(struct Computer2 *comp, struct ComputerTask *ctask)\n{")
opening = source.index("{", start)
depth, end = 1, opening + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
body = source[start:end]
repair = "                    pos = dungeon->essential_pos;\n"
assert body.count(repair) == 1
prefix = r'''
#include <stdio.h>
#include <string.h>
struct Coord3d { int x, y, z; };
struct Thing { int index, owner; };
struct Room { int kind, index; };
struct Dungeon { int owner; struct Coord3d essential_pos; };
struct Computer2 { struct Dungeon *dungeon; int held_thing_idx; };
struct ComputerTask { struct { int room_idx1, room_idx2, repeat_num; } move_to_room; };
struct CreatureModelConfig { int job_primary, job_secondary; };
struct CreatureJobConfig { int func_plyr_check_idx; };
typedef int CreatureJob;
enum { JoKF_AssignComputerDrop=1, JoKF_AssignAreaWithinRoom=2, CrSt_Unused=0,
       CTaskRet_Unk0=0, CTaskRet_Unk1=1, CTaskRet_Unk2=2, Job_NULL=0 };
#define SYNCDBG(...) ((void)0)
#define ERRORLOG(...) (++errors)
#define WARNLOG(...) ((void)0)
static struct Thing creature={19,1};
static struct Room room_value={4,7};
static struct CreatureModelConfig model={1,2};
static struct CreatureJobConfig config={0};
static const struct Coord3d room_pos={111,222,333};
static struct Coord3d observed, forced;
static int allowed, dump_ok, drop_calls, dump_calls, force_calls, removed, errors, pickup_calls;
static struct Room *room_get(int index) { (void)index; return &room_value; }
static struct Thing *thing_get(int index) { return index ? &creature : NULL; }
static int thing_exists(struct Thing *t) { return t != NULL; }
static int thing_is_creature(struct Thing *t) { return t != NULL; }
static int room_is_invalid(struct Room *r) { return r == NULL; }
static int room_exists(struct Room *r) { return r != NULL; }
static struct CreatureModelConfig *creature_stats_get_from_thing(struct Thing *t) { (void)t; return &model; }
static int get_job_for_room(int a,int b,int c) { (void)a;(void)b;(void)c;return 1; }
static struct CreatureJobConfig *get_config_for_job(int job) { (void)job;return &config; }
static int check_job(struct Thing *t,int owner,int job) { (void)t;(void)owner;(void)job;return allowed; }
static int (*creature_job_player_check_func_list[])(struct Thing *,int,int)={check_job};
static int get_drop_position_for_creature_job_in_room(struct Coord3d *p,struct Room *r,int job,struct Thing *t)
{ (void)r;(void)job;(void)t;++drop_calls;*p=room_pos;return 1; }
static int computer_dump_held_things_on_map(struct Computer2 *c,struct Thing *t,struct Coord3d *p,int state)
{ (void)c;(void)t;(void)state;++dump_calls;observed=*p;return dump_ok; }
static void computer_force_dump_held_things_on_map(struct Computer2 *c,struct Coord3d *p)
{ (void)c;++force_calls;forced=*p; }
static void remove_task(struct Computer2 *c,struct ComputerTask *t) { (void)c;(void)t;++removed; }
static int find_creature_to_be_placed_in_room_for_job(struct Computer2 *c,struct Room **r,struct Thing **t)
{ (void)c;*r=&room_value;*t=&creature;return 1; }
static int computer_place_thing_in_power_hand(struct Computer2 *c,struct Thing *t,struct Coord3d *p)
{ (void)c;(void)t;++pickup_calls;observed=*p;return 1; }
'''
suffix = r'''
static int same(struct Coord3d a,struct Coord3d b) { return a.x==b.x && a.y==b.y && a.z==b.z; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(void)
{
    struct Dungeon dungeon={1,{4321,8765,123}};
    struct Computer2 comp={&dungeon,19};
    struct ComputerTask task={{7,8,2}};
    allowed=0;dump_ok=1;
    CHECK(task_move_creature_to_room(&comp,&task)==CTaskRet_Unk2);
    CHECK(same(observed,dungeon.essential_pos));
    CHECK(dump_calls==1 && drop_calls==0 && force_calls==0 && removed==0 && errors==0);
    dump_ok=0;
    CHECK(task_move_creature_to_room(&comp,&task)==CTaskRet_Unk0);
    CHECK(same(observed,dungeon.essential_pos) && same(forced,dungeon.essential_pos));
    CHECK(dump_calls==2 && drop_calls==0 && force_calls==1 && removed==1 && errors==1);
    allowed=1;dump_ok=1;
    CHECK(task_move_creature_to_room(&comp,&task)==CTaskRet_Unk2);
    CHECK(same(observed,room_pos) && drop_calls==1 && dump_calls==3 && force_calls==1 && removed==1);
    comp.held_thing_idx=0;
    CHECK(task_move_creature_to_room(&comp,&task)==CTaskRet_Unk2);
    CHECK(same(observed,room_pos) && pickup_calls==1 && drop_calls==2 && dump_calls==3);
    CHECK(task.move_to_room.repeat_num==1 && task.move_to_room.room_idx2==7);
    puts("PASS actual task body: refused job, failed fallback, accepted job, pickup");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="computer-room-drop-") as temporary:
    directory = Path(temporary)
    for name, function in (("before", body.replace(repair, "")), ("after", body)):
        path = directory / (name + ".c")
        path.write_text(prefix + function + suffix)
        binary = directory / name
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-ftrivial-auto-var-init=pattern",
                        "-fsanitize=address,undefined", "-g", str(path), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        print(name + ": " + result.stdout + result.stderr, end="")
        if name == "before":
            assert result.returncode == 1 and "same(observed,dungeon.essential_pos)" in result.stderr
        else:
            assert result.returncode == 0
