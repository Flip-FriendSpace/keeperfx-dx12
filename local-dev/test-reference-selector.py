# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
"""Run isolated selector-scope CPU controls and production syntax checks in keeperfx-dev.

The harness extracts the actual scope and guard bodies from production source.
Dependencies are minimal read-only context stubs; it does not test geometry or
launch the engine. Generated source and executables stay in private artifacts.
"""
from pathlib import Path
import hashlib
import json
import re
import shlex
import subprocess

repo = Path(__file__).resolve().parent.parent
out = repo.parent / "artifacts" / "reference" / "selector-room-cpu"
out.mkdir(parents=True, exist_ok=True)
engine = (repo / "src/engine_render.c").read_text()
cursor = (repo / "src/cursor_tag.c").read_text()
scope = engine[engine.index("static TbBool reference_selector_enabled;"):
               engine.index("struct EngineReferenceSelectorProof get_engine_reference_selector_proof(void)")]
guard = cursor[cursor.index("TbBool should_draw_cursor_box(PlayerNumber plyr_idx)"):
               cursor.index("unsigned char tag_cursor_blocks_dig(")]
source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef bool TbBool;
typedef int PlayerNumber;
enum { GOF_Paused=1, PSt_CtrlDungeon=3, PCtr_MapCoordsValid=4, PSt_Sell=5, PSt_BuildRoom=6, SLC_RED=0, SLC_GREEN=1, SLC_BROWN=3, SLC_REDYELLOW=5, STL_PER_SLB=3 };
struct RoomSpace { int centreX,centreY,width,height,left,top,plyr_idx,user,rkind,slab_count,invalid_slabs_count,is_active,is_roomspace_a_box,render_roomspace_as_box,is_roomspace_a_single_subtile; };
struct PlayerInfo { int work_state,id_number; struct RoomSpace roomspace,render_roomspace; } player={.work_state=PSt_CtrlDungeon},local_observer_player;
struct UserState { int chosen_room_kind; } user_state={2};
struct MapVolumeBox { int visible,color,beg_x,beg_y,end_x,end_y,floor_height_z; } map_volume_box;
struct EngineReferenceRoomProof { int active,room_kind,busy,valid_slabs,invalid_slabs,colour,hidden_slabs,process_calls,busy_branches,basic_calls,fancy_calls,emitted_lines; };
typedef int RoomKind; typedef int NetUserId; typedef int MapSubtlCoord; typedef int MapSlabCoord;
#define SYNCDBG(...) ((void)0)
#define BFDEBUG_LEVEL 0
struct Camera { int unused; };
struct Packet { int control_flags; };
struct { int operation_flags,map_tiles_x,map_tiles_y,small_map_state; struct { struct { int room_types_count; } slab_conf; } conf; } game={GOF_Paused,85,85,0,{{20}}};
int my_player_number=0,lens_mode=0;
long box_lag_compensation_x=4,box_lag_compensation_y=5;
static bool optin=true,displayed=true,observer=false,busy=true;
static int tag_calls=0,buildable=1,hidden_cells=0,actual_colour=-1;
static struct { int cost; } room_stats={50};
static bool local_camera_reference_enabled(void){return optin;}
static bool flag_is_set(int value,int mask){return (value&mask)!=0;}
static struct PlayerInfo *get_my_player(void){return &player;}
static bool is_player_displayed(int id){(void)id;return displayed;}
static bool is_observer_camera_active(void){return observer;}
static bool game_is_busy_doing_gui(void){return busy;}
static int get_local_user(void){return 0;}
static int calc_distance_from_roomspace_centre(int size,int offset){return (size-1+offset)/2;}
static struct RoomSpace create_box_roomspace(struct RoomSpace r,int w,int h,int x,int y){r.width=w;r.height=h;r.centreX=x;r.centreY=y;r.left=x-(w-1)/2;r.top=y-(h-1)/2;r.slab_count=w*h;r.is_roomspace_a_box=r.render_roomspace_as_box=1;return r;}
static struct RoomSpace check_roomspace_for_diggable_slabs(struct RoomSpace r,int p,void *v){(void)p;(void)v;return r;}
static int slab_subtile_center(int x){return x*3+1;}
static struct PlayerInfo *get_player(int n){(void)n;return &player;}
static struct UserState *get_user_state(int n){(void)n;return &user_state;}
static int get_net_user_player_number(int n){return n;}
static typeof(room_stats) *get_room_kind_stats(int n){(void)n;return &room_stats;}
static int subtile_slab(int n){return n/3;}
static int subtile_coord(int n,int offset){return n*256+offset;}
static int floor_height_for_volume_box(int p,int x,int y){(void)p;(void)x;(void)y;return 1;}
static int subtile_revealed(int x,int y,int p){(void)y;(void)p;return !(hidden_cells&&x/3==11);}
static struct RoomSpace check_slabs_in_roomspace(struct RoomSpace r,int cost){(void)cost;r.slab_count=buildable;r.invalid_slabs_count=r.width*r.height-buildable;r.is_roomspace_a_box=(buildable==0||buildable==r.width*r.height);return r;}
static int can_build_roomspace(int p,int k,struct RoomSpace r){assert(p==0&&k==2);(void)r;return buildable;}
TbBool tag_cursor_blocks_place_roomspace(PlayerNumber,RoomKind,struct RoomSpace*,MapSubtlCoord,MapSubtlCoord,TbBool);
TbBool should_draw_cursor_box(PlayerNumber plyr_idx);
static void tag_cursor_blocks_dig(struct PlayerInfo *p,int user,const struct Packet *packet,struct RoomSpace *r,int x,int y,bool full){
 (void)p;(void)user;(void)r;(void)x;(void)y;(void)full;tag_calls++;
 assert(packet->control_flags==PCtr_MapCoordsValid);
 if(should_draw_cursor_box(my_player_number)){map_volume_box.visible=1;map_volume_box.color=1;}
}
''' + scope + guard + cursor[cursor.index("TbBool tag_cursor_blocks_place_room(NetUserId"):
                               cursor.index("void tag_cursor_blocks_place_terrain(")] + engine[engine.index("static TbBool map_volume_box_build_in_progress("):
                               engine.index("static TbBool reference_hover_enabled")] + r'''
static struct RoomSpace *get_local_dig_prediction_render_roomspace(struct RoomSpace *r){return r;}
static void create_map_volume_box(long x,long y,long z,int colour){(void)x;(void)y;(void)z;actual_colour=colour;}
static void create_fancy_map_volume_box(struct RoomSpace r,long x,long y,long z,int colour,bool outer){(void)r;(void)x;(void)y;(void)z;(void)outer;actual_colour=colour;}
static void create_frontview_map_volume_box(struct Camera *c,unsigned char w,int colour){(void)c;(void)w;actual_colour=colour;}
static void create_fancy_frontview_map_volume_box(struct RoomSpace r,struct Camera *c,unsigned char w,int colour,bool outer){(void)r;(void)c;(void)w;(void)outer;actual_colour=colour;}
''' + engine[engine.index("static void process_isometric_map_volume_box("):
             engine.index("static void do_a_trig_gourad_tr(")] + engine[engine.index("static void process_frontview_map_volume_box("):
             engine.index("TbBool cursor_on_room(")] + r'''
int main(void){
 int checks=0;
#define CHECK(v) do{assert(v);checks++;}while(0)
 CHECK(!should_draw_cursor_box(0));
 busy=false;CHECK(should_draw_cursor_box(0));busy=true;
 CHECK(set_engine_reference_selector(10,10,1,1));
 CHECK(!should_draw_cursor_box(0)); // configured is not drawing
 map_volume_box=(struct MapVolumeBox){.color=7,.beg_x=123};
 const struct MapVolumeBox saved=map_volume_box;
 begin_engine_reference_selector();
 CHECK(reference_selector_drawing);CHECK(map_volume_box.visible);
 CHECK(should_draw_cursor_box(0));CHECK(!should_draw_cursor_box(1));
 CHECK(busy&&player.work_state==PSt_CtrlDungeon&&game.operation_flags==GOF_Paused);
 displayed=false;CHECK(!should_draw_cursor_box(0));displayed=true;
 game.small_map_state=2;CHECK(!should_draw_cursor_box(0));game.small_map_state=0;
 observer=true;CHECK(!should_draw_cursor_box(0));observer=false;
 end_engine_reference_selector();
 CHECK(!reference_selector_drawing);CHECK(!should_draw_cursor_box(0));
 CHECK(memcmp(&saved,&map_volume_box,sizeof(saved))==0);
 CHECK(box_lag_compensation_x==4&&box_lag_compensation_y==5);
 clear_engine_reference_selector();begin_engine_reference_selector();CHECK(!reference_selector_drawing);
 const int bad[][4]={{10,10,0,1},{10,10,10,1},{10,10,1,0},{10,10,1,10},{-1,10,1,1},{85,10,1,1},{10,-1,1,1},{10,85,1,1},{0,0,9,9},{84,84,9,9}};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){
  CHECK(!set_engine_reference_selector(bad[i][0],bad[i][1],bad[i][2],bad[i][3]));
  begin_engine_reference_selector();CHECK(!reference_selector_drawing);CHECK(!should_draw_cursor_box(0));
 }
 for(int i=0;i<4;i++){
  CHECK(set_engine_reference_selector(10,10,1,1));
  if(i==0)optin=false;if(i==1)game.operation_flags=0;if(i==2)player.work_state=99;if(i==3)lens_mode=1;
  if(i<3)CHECK(!set_engine_reference_selector(10,10,1,1));
  const int before=tag_calls;begin_engine_reference_selector();
  CHECK(!reference_selector_drawing&&!reference_selector_enabled);CHECK(tag_calls==before);CHECK(!should_draw_cursor_box(0));
  optin=true;game.operation_flags=GOF_Paused;player.work_state=PSt_CtrlDungeon;lens_mode=0;
 }
 CHECK(set_engine_reference_selector(10,10,1,1));begin_engine_reference_selector();
 CHECK(!set_engine_reference_selector(11,11,1,1));end_engine_reference_selector();clear_engine_reference_selector();
 CHECK(!should_draw_cursor_box(0));CHECK(busy);
 const struct PlayerInfo saved_player=player; const struct UserState saved_user=user_state;
 for(int front=0;front<2;front++)for(int b=0;b<2;b++)for(int valid=0;valid<2;valid++){
  buildable=valid;CHECK(set_engine_reference_room_preview(2,10,10,1,1,b));begin_engine_reference_selector();
  CHECK(reference_room_proof.active&&reference_room_proof.valid_slabs==valid);CHECK(map_volume_box.color==valid);
  if(front)process_frontview_map_volume_box(NULL,4,0);else process_isometric_map_volume_box(0,0,0,0);
  CHECK(reference_room_proof.process_calls==1&&reference_room_proof.basic_calls==1);
  CHECK(reference_room_proof.busy_branches==b);CHECK(actual_colour==(b?SLC_REDYELLOW:valid));
  CHECK(!reference_room_emitting);CHECK(memcmp(&player,&saved_player,sizeof(player))==0);CHECK(memcmp(&user_state,&saved_user,sizeof(user_state))==0);
  end_engine_reference_selector();CHECK(memcmp(&map_volume_box,&saved,sizeof(saved))==0);CHECK(box_lag_compensation_x==4&&box_lag_compensation_y==5);
 }
 buildable=6;hidden_cells=1;CHECK(set_engine_reference_room_preview(2,10,10,3,3,0));begin_engine_reference_selector();
 CHECK(reference_room_proof.valid_slabs==6&&reference_room_proof.invalid_slabs==3&&reference_room_proof.hidden_slabs==3);
 process_isometric_map_volume_box(0,0,0,0);CHECK(reference_room_proof.fancy_calls==1);end_engine_reference_selector();
 clear_engine_reference_selector();CHECK(!set_engine_reference_room_preview(0,10,10,1,1,0));CHECK(!set_engine_reference_room_preview(20,10,10,1,1,0));CHECK(!set_engine_reference_room_preview(2,10,10,1,1,2));
 for(int context=0;context<3;context++){
  CHECK(set_engine_reference_room_preview(2,10,10,1,1,1));
  if(context==0)optin=false;if(context==1)game.operation_flags=0;if(context==2)player.work_state=99;
  CHECK(!set_engine_reference_room_preview(2,10,10,1,1,1));begin_engine_reference_selector();CHECK(!reference_selector_drawing);
  optin=true;game.operation_flags=GOF_Paused;player.work_state=PSt_CtrlDungeon;
 }
 busy=false;buildable=1;player.render_roomspace=create_box_roomspace((struct RoomSpace){0},1,1,10,10);player.render_roomspace.is_roomspace_a_single_subtile=true;
 CHECK(tag_cursor_blocks_place_room(0,31,31,true));CHECK(!player.render_roomspace.is_roomspace_a_single_subtile);
 player.work_state=PSt_BuildRoom;player.roomspace.is_active=true;
 process_isometric_map_volume_box(0,0,0,0);CHECK(actual_colour==SLC_REDYELLOW);
 printf("%d selector/room scope and original cursor/geometry-branch CPU checks passed\n",checks);return 0;
}
'''
generated = out / "selector-guard-test.c"
generated.write_text(source)
exe = out / "selector-guard-test"
subprocess.run(["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
                str(generated), "-o", str(exe)], check=True)
result = subprocess.check_output([str(exe)], text=True)
print(result, end="")
checked = []
for architecture, build, triple in (
    ("x64", Path("/opt/keeperfx-build/x64/RelWithDebInfo"), "x86_64-w64-mingw32"),
    ("x86", Path("/opt/keeperfx-build/RelWithDebInfo"), "i686-w64-mingw32"),
):
    ninja = (build / "build.ninja").read_text()
    for name in ("src/cursor_tag.c", "src/engine_render.c", "src/console_cmd.c", "src/kfx/renderer/SceneAssetCapture.cpp"):
        match = re.search(r"^build CMakeFiles/keeperfx\.dir/" + re.escape(name) + r"\.obj:.*\n((?:  .*\n)+)", ninja, re.M)
        if not match:
            raise RuntimeError("Missing existing production compile flags: " + name)
        fields = dict(re.findall(r"^  (DEFINES|FLAGS|INCLUDES) = (.*)$", match[1], re.M))
        flags = shlex.split(" ".join(fields[key] for key in ("DEFINES", "FLAGS", "INCLUDES")))
        subprocess.run([triple + ("-gcc" if name.endswith(".c") else "-g++"), *flags,
                        "-fsyntax-only", str(repo / name)], check=True, cwd=build)
        checked.append(architecture + ":" + name)
        print("syntax passed", checked[-1])
report = {"cpu":result.strip(), "productionSyntax":checked,
          "extractedSourceSha256":hashlib.sha256(source.encode()).hexdigest(),
          "sourceHashes":{name:hashlib.sha256((repo/name).read_bytes()).hexdigest() for name in
                          ("src/cursor_tag.c", "src/engine_render.c", "src/engine_render.h", "src/kfx/renderer/SceneAssetCapture.cpp")}}
(out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
