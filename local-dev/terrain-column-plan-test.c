// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/TerrainColumnPlan.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expected_add(struct TerrainColumnPlan* p,unsigned kind,unsigned height,unsigned texture,unsigned unlit,unsigned remap)
{
    assert(p->count<37);
    p->operations[p->count++]=(struct TerrainColumnOperation){(uint16_t)texture,(uint8_t)kind,(uint8_t)height,(uint8_t)unlit,(uint8_t)remap};
}
/* The old walk's observable face-call sequence, before projected emitters. */
static struct TerrainColumnPlan legacy(const struct TerrainColumnInputs* in)
{
    struct TerrainColumnPlan p={0};
    unsigned mask=1,height=0;
    while(mask<=in->masks[0]) {
        if(mask&in->masks[0]) {
            if(!(mask&in->masks[1]))expected_add(&p,TCP_Back,height,in->sides[height][0],0,0);
            if(!(mask&in->masks[2]))expected_add(&p,TCP_Front,height,in->sides[height][1],0,0);
            if(!(mask&in->masks[3]))expected_add(&p,TCP_Left,height,in->sides[height][2],0,0);
            if(!(mask&in->masks[4]))expected_add(&p,TCP_Right,height,in->sides[height][3],0,0);
        }
        mask*=2;++height;
    }
    expected_add(&p,TCP_Abyss,0,0,0,0);
    if(in->mode==TCP_Perspective) {
        if(in->floor_height>0)expected_add(&p,TCP_Top,in->floor_height,in->top_texture,0,1);
        else if(!in->floor_abyss)expected_add(&p,TCP_Top,0,in->floor_texture,0,1);
        if(in->lintel_height>0) {
            expected_add(&p,TCP_Top,in->lintel_height,in->lintel_texture,0,1);
            expected_add(&p,TCP_Bottom,in->lintel_bottom_height,in->lintel_bottom_texture,0,0);
        }
        if(in->draw_ceiling)expected_add(&p,TCP_Bottom,in->ceiling_height,in->ceiling_texture,0,0);
    } else {
        if(in->floor_height>0 && (in->mode!=TCP_Cluedo || in->floor_height<=8)) {
            if(in->unexplored)expected_add(&p,TCP_Top,in->floor_height,in->marked_land,1,0);
            else if(in->mode==TCP_Cluedo) {
                if(in->tagged_valuable)expected_add(&p,TCP_Top,in->floor_height,in->marked_gold,1,0);
                else if(in->raw_floor_height>0&&in->raw_floor_height<=8)
                    expected_add(&p,TCP_Top,in->floor_height,in->top_texture,0,1);
            } else if(!(in->tagged_valuable||in->unexplored))expected_add(&p,TCP_Top,in->floor_height,in->top_texture,0,1);
            else if(in->valuable)expected_add(&p,TCP_Top,in->floor_height,in->marked_gold,1,0);
        } else if(!in->floor_abyss) {
            if(!in->unexplored)expected_add(&p,TCP_Top,0,in->floor_texture,0,1);
            else expected_add(&p,TCP_Top,0,in->marked_land,1,0);
        }
        if(in->lintel_height>0 && (in->mode!=TCP_Cluedo || in->lintel_height<=8))expected_add(&p,TCP_Top,in->lintel_height,in->lintel_texture,0,1);
    }
    return p;
}
static void compare(const struct TerrainColumnPlan* a,const struct TerrainColumnPlan* b)
{
    assert(a->count==b->count);
    for(unsigned i=0;i<a->count;++i){const struct TerrainColumnOperation *x=&a->operations[i],*y=&b->operations[i];
        assert(x->kind==y->kind&&x->height==y->height&&x->texture==y->texture&&x->unlit==y->unlit&&x->top_remap==y->top_remap);}
}
int main(void)
{
    struct TerrainColumnInputs in={0};
    in.floor_texture=11;in.top_texture=12;in.lintel_texture=13;in.marked_land=578;in.marked_gold=579;
    for(unsigned h=0;h<8;++h)for(unsigned side=0;side<4;++side)in.sides[h][side]=(uint16_t)(100+h*4+side);
    unsigned checked=0;
    for(unsigned mode=0;mode<TCP_ModeCount;++mode)for(unsigned mask=0;mask<256;++mask)for(unsigned neighbours=0;neighbours<256;++neighbours)for(unsigned flags=0;flags<8;++flags) {
        in.mode=(uint8_t)mode;
        in.raw_floor_height=(uint8_t)(neighbours%9);
        in.lintel_bottom_height=(uint8_t)(neighbours%8);
        in.ceiling_height=9;in.draw_ceiling=(uint8_t)((neighbours>>1)&1);
        in.lintel_bottom_texture=91;in.ceiling_texture=92;
        in.masks[0]=(uint16_t)mask;
        in.masks[1]=(uint16_t)neighbours;in.masks[2]=(uint16_t)(255-neighbours);
        in.masks[3]=(uint16_t)((neighbours<<1)&255);in.masks[4]=(uint16_t)(neighbours>>1);
        in.floor_height=0;while(in.floor_height<8&&(mask&(1u<<in.floor_height)))++in.floor_height;
        in.lintel_height=(uint8_t)((neighbours%9));in.floor_abyss=(uint8_t)(neighbours&1);
        in.unexplored=(uint8_t)(flags&1);in.tagged_valuable=(uint8_t)((flags>>1)&1);in.valuable=(uint8_t)((flags>>2)&1);
        const struct TerrainColumnPlan reference=legacy(&in);
        compare(TerrainColumnPlanResolve(1,16,16,&in),&reference);
        const struct TerrainColumnPlanStats before=TerrainColumnPlanGetStats();
        compare(TerrainColumnPlanResolve(1,16,16,&in),&reference);
        assert(TerrainColumnPlanGetStats().hits==before.hits+1);++checked;
    }
    /* Every field is compared explicitly, including neighbours and material changes. */
    const struct TerrainColumnPlanStats original=TerrainColumnPlanGetStats();
    unsigned changes=0;
    for(unsigned i=0;i<5;++i){in.masks[i]^=1;TerrainColumnPlanResolve(1,16,16,&in);++changes;}
    for(unsigned i=0;i<8;++i)for(unsigned j=0;j<4;++j){++in.sides[i][j];TerrainColumnPlanResolve(1,16,16,&in);++changes;}
#define CHANGE(field) do { ++in.field; TerrainColumnPlanResolve(1,16,16,&in); ++changes; } while(0)
    CHANGE(floor_texture);CHANGE(top_texture);CHANGE(lintel_texture);CHANGE(marked_land);CHANGE(marked_gold);CHANGE(lintel_bottom_texture);CHANGE(ceiling_texture);
#undef CHANGE
#define CHANGE(field) do { in.field^=1; TerrainColumnPlanResolve(1,16,16,&in); ++changes; } while(0)
    CHANGE(floor_height);CHANGE(lintel_height);CHANGE(floor_abyss);CHANGE(unexplored);CHANGE(tagged_valuable);CHANGE(valuable);CHANGE(raw_floor_height);CHANGE(lintel_bottom_height);CHANGE(ceiling_height);CHANGE(draw_ceiling);
    in.mode=TCP_Isometric;TerrainColumnPlanResolve(1,16,16,&in);++changes;
#undef CHANGE
    assert(TerrainColumnPlanGetStats().builds==original.builds+changes);
    const struct TerrainColumnPlanStats old=TerrainColumnPlanGetStats();
    TerrainColumnPlanResolve(2,16,16,&in);
    assert(TerrainColumnPlanGetStats().builds==old.builds+1&&TerrainColumnPlanGetStats().epoch_resets==old.epoch_resets+1);
    for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x)TerrainColumnPlanResolve(2,(uint16_t)x,(uint16_t)y,&in);
    const struct TerrainColumnPlanStats final=TerrainColumnPlanGetStats();
    for(unsigned mode=0;mode<TCP_ModeCount;++mode)assert(final.mode_hits[mode]>=524288 && final.mode_builds[mode]>=524288);
    assert(final.evictions>0&&final.capacity_entries==16384&&final.capacity_bytes==old.capacity_bytes);
    printf("PASS %u legacy recipe comparisons; %u dependency mutations; fixed %zu bytes; epoch and eviction verified\n",checked,changes,final.capacity_bytes);
    return 0;
}
