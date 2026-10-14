// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/FrontTerrainPlan.h"
#include <assert.h>
#include <stdio.h>

struct Event { int kind, height, y, texture, marked; };
struct Trace { unsigned count; int ymax; struct Event events[10]; };
static void emit(struct Trace* out,int kind,int height,int y,int texture,int marked)
{
    assert(out->count<10);
    out->events[out->count++]=(struct Event){kind,height,y,texture,marked};
}
/* Original draw_element cube-loop control flow, with only final quad calls
 * replaced by observable events. y/max mutation is independent of the plan. */
static struct Trace original(const struct FrontTerrainInputs* in,int pos,int zoom,int ymax,unsigned flags)
{
    struct Trace out={0}; out.ymax=ymax;
    int tc,y=zoom+pos,last=-1,delta=(zoom<<7)/256;
    for(tc=0;tc<8;tc++) {
        if(!in->present[tc])break;
        y-=delta;last=tc;
        if(out.ymax>y){out.ymax=y;emit(&out,FTP_Side,tc+1,y,in->side_texture[tc],0);}
    }
    if(last!=-1) {
        int i=y-zoom;
        if(out.ymax>i){out.ymax=i;emit(&out,FTP_Top,tc,i,in->top_texture[last],flags&1?2:(flags&2?1:0));}
    }
    if(in->ceiling_filled&&in->solidmask>(1u<<tc)) {
        for(;tc<8;tc++) {if(in->present[tc])break;y-=delta;}
        for(;tc<8;tc++) {
            if(!in->present[tc])break;
            y-=delta;last=tc;
            if(out.ymax>y)emit(&out,FTP_Side,tc+1,y,in->side_texture[tc],0);
        }
        if(last!=-1) {
            int i=y-zoom;
            if(out.ymax>i)emit(&out,FTP_Top,tc,i,in->top_texture[last],0);
        }
    }
    return out;
}
static struct Trace replay(const struct FrontTerrainPlan* plan,int pos,int zoom,int ymax,unsigned flags)
{
    struct Trace out={0};out.ymax=ymax;
    unsigned height=0;int y=zoom+pos,delta=(zoom<<7)/256;
    for(unsigned i=0;i<plan->count;++i){const struct FrontTerrainOperation* op=&plan->operations[i];
        while(height<op->height){y-=delta;++height;}
        int draw_y=op->kind==FTP_Top?y-zoom:y;
        if(out.ymax<=draw_y)continue;
        if(op->lower_run)out.ymax=draw_y;
        int marked=op->kind==FTP_Top&&op->lower_run?(flags&1?2:(flags&2?1:0)):0;
        emit(&out,op->kind,op->height,draw_y,op->texture,marked);
    }
    return out;
}
static void compare(const struct Trace* a,const struct Trace* b)
{
    assert(a->count==b->count&&a->ymax==b->ymax);
    for(unsigned i=0;i<a->count;++i){const struct Event *x=&a->events[i],*y=&b->events[i];
        assert(x->kind==y->kind&&x->height==y->height&&x->y==y->y&&x->texture==y->texture&&x->marked==y->marked);}
}
int main(void)
{
    struct FrontTerrainInputs in={0};unsigned checks=0;
    for(unsigned mask=0;mask<256;++mask)for(unsigned solid=0;solid<512;++solid)for(unsigned ceiling=0;ceiling<2;++ceiling){
        in.solidmask=(uint16_t)solid;in.ceiling_filled=(uint8_t)ceiling;
        for(unsigned h=0;h<8;++h){in.present[h]=(uint8_t)((mask>>h)&1);in.side_texture[h]=(uint16_t)(100+h);in.top_texture[h]=(uint16_t)(200+h);}
        const struct FrontTerrainPlan* plan=FrontTerrainPlanResolve(10,20,21,&in);
        for(unsigned variant=0;variant<8;++variant){
            const int zoom=127+(int)variant*19,pos=-71+(int)variant*17;
            const int ymax=variant<4?pos+zoom-(int)variant*(zoom/2):pos-(int)(variant-3)*zoom;
            const struct Trace expected=original(&in,pos,zoom,ymax,variant&3);
            const struct Trace actual=replay(plan,pos,zoom,ymax,variant&3);compare(&expected,&actual);++checks;
        }
        const struct FrontTerrainPlanStats before=FrontTerrainPlanGetStats();
        FrontTerrainPlanResolve(10,20,21,&in);assert(FrontTerrainPlanGetStats().hits==before.hits+1);
    }
    FrontTerrainPlanResolve(11,20,21,&in);
    struct FrontTerrainPlanStats before=FrontTerrainPlanGetStats();unsigned changes=0;
    for(unsigned h=0;h<8;++h){in.present[h]^=1;FrontTerrainPlanResolve(11,20,21,&in);++changes;
        ++in.side_texture[h];FrontTerrainPlanResolve(11,20,21,&in);++changes;
        ++in.top_texture[h];FrontTerrainPlanResolve(11,20,21,&in);++changes;}
    in.solidmask^=1;FrontTerrainPlanResolve(11,20,21,&in);++changes;
    in.ceiling_filled^=1;FrontTerrainPlanResolve(11,20,21,&in);++changes;
    assert(FrontTerrainPlanGetStats().builds==before.builds+changes);
    before=FrontTerrainPlanGetStats();FrontTerrainPlanResolve(2,20,21,&in);
    assert(FrontTerrainPlanGetStats().builds==before.builds+1&&FrontTerrainPlanGetStats().epoch_resets==before.epoch_resets+1);
    /* A full extended-map sweep shares the same bounded template repertoire.
     * Four quadrant material profiles,1024 distinct raw templates per profile. */
    for(unsigned pass=0;pass<2;++pass){
        struct FrontTerrainPlanStats warm={0};
        if(pass)warm=FrontTerrainPlanGetStats();
        for(unsigned quadrant=0;quadrant<4;++quadrant)
            for(unsigned row=0;row<511;++row)for(unsigned col=0;col<511;++col){
                const unsigned x=(quadrant&1)?510-col:col,y=(quadrant&2)?510-row:row;
                const unsigned pattern=(y*511+x)%1024;
                in.side_texture[0]=(uint16_t)(pattern+quadrant*1024);in.top_texture[0]=(uint16_t)pattern;
                FrontTerrainPlanResolve(3,(uint16_t)x,(uint16_t)y,&in);
            }
        if(pass){const struct FrontTerrainPlanStats after=FrontTerrainPlanGetStats();
            assert(after.builds==warm.builds&&after.evictions==warm.evictions);
            assert(after.hits==warm.hits+4ull*511*511);}
    }
    const struct FrontTerrainPlanStats warm=FrontTerrainPlanGetStats();
    in.side_texture[0]=65530;
    const struct Trace expected=original(&in,0,256,4096,0);
    const struct Trace changed=replay(FrontTerrainPlanResolve(3,510,510,&in),0,256,4096,0);
    compare(&expected,&changed);
    const struct Trace shared=replay(FrontTerrainPlanResolve(3,65535,65535,&in),0,256,4096,0);
    compare(&expected,&shared);
    const struct FrontTerrainPlanStats final=FrontTerrainPlanGetStats();
    assert(final.builds==warm.builds+1&&final.hits==warm.hits+1&&final.evictions==warm.evictions);
    assert(final.capacity_entries==16384&&final.capacity_bytes==before.capacity_bytes);
    printf("PASS %u original screen-emitter traces; %u dependencies; 4 complete511x511 template scans zero rebuilds/evictions; coordinate-independent exact reuse; fixed %zu bytes\n",checks,changes,final.capacity_bytes);
    return 0;
}
