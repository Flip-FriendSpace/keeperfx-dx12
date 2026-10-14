# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
"""Focused CPU controls for extracted production hover scope/remap and identity.

Run in keeperfx-dev. No game, GPU, or full-engine build is performed.
"""
from pathlib import Path
import hashlib
import json
import re
import shlex
import subprocess

repo = Path(__file__).resolve().parent.parent
out = repo.parent / "artifacts" / "reference" / "hover-cpu"
out.mkdir(parents=True, exist_ok=True)
engine = (repo / "src/engine_render.c").read_text()
header = (repo / "src/engine_render.h").read_text()
proof = header[header.index("struct EngineReferenceHoverProof {"):header.index("TbBool set_engine_reference_hover(")]
scope = engine[engine.index("static TbBool reference_hover_enabled,"):engine.index("long view_height_over_2;")]
start = engine.index("void draw_fastview_mapwho(")
start = engine.index("    if ((thing->class_id == TCls_Creature)", start)
end = engine.index("    record_engine_reference_hover_remap(thing);", start)+len("    record_engine_reference_hover_remap(thing);")
branch = engine[start:end]
source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef bool TbBool;
enum{GOF_Paused=1,PSt_CtrlDungeon=3,PSt_QueryAll=4,PLAYERS_COUNT=9,THINGS_COUNT=8,TCls_Creature=5,TCls_Object=1,TCls_DeadCreature=8,Lb_SPRITE_REMAP=0x800};
struct PlayerInfo{int work_state;}player={PSt_CtrlDungeon};
struct Thing{int index,exists,class_id,owner,model,creation_turn,last_turn_damaged,rendering_flags;struct{struct{struct{int num;}stl;}x,y;}mappos;}things[THINGS_COUNT];
struct{int operation_flags,map_subtiles_x,map_subtiles_y;}game={GOF_Paused,255,255};
struct{int local_thing_under_hand;}local_state={3};
static bool optin=true;static int lens_mode=0,turn=77,gui_blink_rate=1;
static uint64_t incarnations[THINGS_COUNT];
static unsigned flags;static unsigned char white_pal[256],red_pal[256],*lbSpriteReMapPtr;
static int thing_being_displayed_is_creature;static struct Thing *thing_being_displayed;
static bool local_camera_reference_enabled(void){return optin;}
static bool flag_is_set(int v,int m){return (v&m)!=0;}
static struct PlayerInfo *get_my_player(void){return &player;}
static struct Thing *thing_get(int i){return &things[i>0&&i<THINGS_COUNT?i:0];}
static bool thing_exists(const struct Thing *t){return t->exists;}
static uint64_t WorldSceneThingIncarnation(unsigned i){return i<THINGS_COUNT?incarnations[i]:0;}
static int get_gameturn(void){return turn;}
static unsigned RendererGetDrawFlags(void){return flags;}
static void RendererAddDrawFlags(unsigned f){flags|=f;}
''' + proof + scope + r'''
static void actual_front_remap(struct Thing *thing){const struct PlayerInfo *player=get_my_player();
''' + branch + r'''
}
int main(void){int checks=0;
#define CHECK(v) do{assert(v);checks++;}while(0)
 struct Thing *target=&things[1];
 *target=(struct Thing){.index=1,.exists=1,.class_id=TCls_Creature,.owner=0,.model=6,.creation_turn=12,.last_turn_damaged=77};
 target->mappos.x.stl.num=199;target->mappos.y.stl.num=193;incarnations[1]=10;
 memset(white_pal,1,256);memset(red_pal,2,256);
 const struct Thing saved=*target;
 for(int hovered=0;hovered<2;hovered++)for(int blink=0;blink<2;blink++)for(int hit=0;hit<2;hit++){
  target->last_turn_damaged=hit?77:76;flags=0;lbSpriteReMapPtr=NULL;
  CHECK(set_engine_reference_hover(0,6,199,193,hovered,blink));
  CHECK(!reference_hover_drawing);CHECK(!render_thing_is_hovered(target));
  begin_engine_reference_hover();CHECK(reference_hover_drawing);
  actual_front_remap(target);const struct EngineReferenceHoverProof p=get_engine_reference_hover_proof();
  CHECK(p.active==1&&p.actual_hit==hit&&p.considered==1&&p.target_incarnation==10);
  CHECK(p.white==(hovered&&blink));CHECK(p.red==(!(hovered&&blink)&&hit));CHECK(p.normal==(!(hovered&&blink)&&!hit));
  CHECK(p.white_remap[0]==1&&p.red_remap[0]==2);
  CHECK(local_state.local_thing_under_hand==3&&turn==77&&target->creation_turn==saved.creation_turn);
  end_engine_reference_hover();CHECK(!get_engine_reference_hover_proof().active);
 }
 target->last_turn_damaged=77;CHECK(memcmp(&saved,target,sizeof(saved))==0);
 clear_engine_reference_hover();local_state.local_thing_under_hand=1;
 flags=0;actual_front_remap(target);CHECK(lbSpriteReMapPtr==red_pal); // actual blink is dim
 turn=78;flags=0;actual_front_remap(target);CHECK(lbSpriteReMapPtr==white_pal);turn=77;local_state.local_thing_under_hand=3;
 for(int i=0;i<4;i++){
  CHECK(set_engine_reference_hover(0,6,199,193,1,1));
  if(i==0)optin=false;if(i==1)game.operation_flags=0;if(i==2)player.work_state=99;if(i==3)lens_mode=1;
  if(i<3)CHECK(!set_engine_reference_hover(0,6,199,193,1,1));
  begin_engine_reference_hover();CHECK(!reference_hover_drawing&&!reference_hover_enabled);
  optin=true;game.operation_flags=GOF_Paused;player.work_state=PSt_CtrlDungeon;lens_mode=0;
 }
 CHECK(!set_engine_reference_hover(0,6,199,193,2,1));CHECK(!set_engine_reference_hover(0,6,199,193,1,2));
 CHECK(!set_engine_reference_hover(0,6,255,193,1,1));CHECK(!set_engine_reference_hover(0,99,199,193,1,1));
 things[2]=*target;things[2].index=2;incarnations[2]=11;CHECK(!set_engine_reference_hover(0,6,199,193,1,1));things[2].exists=0;
 CHECK(set_engine_reference_hover(0,6,199,193,1,1));incarnations[1]=12;begin_engine_reference_hover();CHECK(!reference_hover_drawing&&!reference_hover_enabled);
 incarnations[1]=0;CHECK(!set_engine_reference_hover(0,6,199,193,1,1));
 incarnations[1]=13;
 for(int i=0;i<4;i++){
  CHECK(set_engine_reference_hover(0,6,199,193,1,1));
  if(i==0)target->owner=1;if(i==1)target->model=16;if(i==2)target->class_id=TCls_Object;if(i==3)target->creation_turn=13;
  begin_engine_reference_hover();CHECK(!reference_hover_drawing&&!reference_hover_enabled);*target=saved;
 }
 incarnations[1]=13;CHECK(set_engine_reference_hover(0,6,199,193,1,1));target->exists=0;begin_engine_reference_hover();CHECK(!reference_hover_drawing);target->exists=1;
 CHECK(set_engine_reference_hover(0,6,199,193,1,1));clear_engine_reference_hover();begin_engine_reference_hover();CHECK(!reference_hover_drawing);
 CHECK(local_state.local_thing_under_hand==3&&turn==77&&game.operation_flags==GOF_Paused);
 printf("%d actual hover scope/front-remap CPU controls passed\n",checks);return 0;
}
'''
generated = out / "hover-scope-test.c"
generated.write_text(source)
exe = out / "hover-scope-test"
subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation", str(generated), "-o", str(exe)], check=True)
results = [subprocess.check_output([str(exe)], text=True).strip()]
identity = out / "hover-identity-test"
subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+str(repo/"src"),
                str(repo/"local-dev/reference-hover-identity-test.cpp"), "-o", str(identity)], check=True)
results.append(subprocess.check_output([str(identity)], text=True).strip())
for result in results:
    print(result, flush=True)
names=("src/engine_render.c", "src/console_cmd.c", "src/local_camera.c", "src/kfx/renderer/WorldSceneCapture.cpp", "src/kfx/renderer/SceneAssetCapture.cpp")
checked=[]
for architecture, build, triple in (("x64",Path("/opt/keeperfx-build/x64/RelWithDebInfo"),"x86_64-w64-mingw32"),("x86",Path("/opt/keeperfx-build/RelWithDebInfo"),"i686-w64-mingw32")):
    ninja=(build/"build.ninja").read_text()
    for name in names:
        match=re.search(r"^build CMakeFiles/keeperfx\.dir/"+re.escape(name)+r"\.obj:.*\n((?:  .*\n)+)",ninja,re.M)
        if not match:
            raise RuntimeError("Missing existing production flags: "+name)
        fields=dict(re.findall(r"^  (DEFINES|FLAGS|INCLUDES) = (.*)$",match[1],re.M))
        flags=shlex.split(" ".join(fields[key] for key in ("DEFINES","FLAGS","INCLUDES")))
        subprocess.run([triple+("-gcc" if name.endswith(".c") else "-g++"),*flags,"-fsyntax-only",str(repo/name)],check=True,cwd=build)
        checked.append(architecture+":"+name)
        print("syntax passed",checked[-1],flush=True)
(out/"report.json").write_text(json.dumps({"cpu":results,"productionSyntax":checked,
    "extractedSourceSha256":hashlib.sha256(source.encode()).hexdigest(),
    "sourceHashes":{name:hashlib.sha256((repo/name).read_bytes()).hexdigest() for name in names}},indent=2)+"\n")
