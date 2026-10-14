#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
"""Compile exact production C function bodies with isolated file/heap fixtures.
No engine/GPU run. The emitted fixture hash binds the extracted functions;
full translation-unit checks remain separate from these source-bound tests.
"""
import argparse, hashlib, json, pathlib, subprocess
p=argparse.ArgumentParser(); p.add_argument('--compiler',default='gcc'); p.add_argument('--output',required=True)
a=p.parse_args(); root=pathlib.Path(__file__).resolve().parents[1]; out=pathlib.Path(a.output); out.mkdir(parents=True,exist_ok=True)
def function(path,name,text=None):
    if text is None: text=(root/path).read_text()
    lines=text.splitlines(True)
    for i,line in enumerate(lines):
        if line and not line[0].isspace() and '(' in line and name+'(' in line and not line.rstrip().endswith(';'):
            start=sum(map(len,lines[:i])); body=text.index('{',start); depth=1; end=body+1
            while depth:
                depth+=(text[end]=='{')-(text[end]=='}'); end+=1
            return text[start:end]+'\n'
    raise RuntimeError('Function missing: '+name)
engine='src/engine_render.c'; custom='src/custom_sprites.c'
functions=[function(engine,n) for n in ['load_single_frame','get_keepersprite_source_extent']]
functions += [function(custom,n) for n in ['sprite_rle_capacity','convert_row','compress_raw']]
legacy_text=subprocess.run(['git','show','HEAD:src/custom_sprites.c'],cwd=root,capture_output=True,text=True,check=True).stdout
legacy=function(custom,'compress_raw',legacy_text).replace('compress_raw(', 'legacy_compress_raw(',1)
functions.append(legacy)
shim=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#define KEEPSPRITE_LENGTH 9149
#define KEEPERSPRITE_ADD_OFFSET 16384
#define KEEPERSPRITE_ADD_NUM 16383
#define Lb_FILE_SEEK_BEGINNING 0
#define MAX_COLOR_VALUE 64
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"Failed line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
typedef unsigned char* TbSpriteData;
typedef int TbBool;
struct KeeperSprite {uint32_t DataOffset;};
struct TbHugeSprite {unsigned char* Data; int32_t* Lines; unsigned long SWidth,SHeight;};
static struct KeeperSprite entries[4];
static struct KeeperSprite* creature_table=entries;
static size_t creature_table_length=4;
static TbSpriteData sprite_heap_handle[KEEPSPRITE_LENGTH];
static TbSpriteData* keepsprite[KEEPSPRITE_LENGTH];
static size_t keeper_sprite_loaded_bytes[KEEPSPRITE_LENGTH];
static TbSpriteData keepersprite_add[KEEPERSPRITE_ADD_NUM];
static size_t keepersprite_add_bytes[KEEPERSPRITE_ADD_NUM];
static int file_token;
static void* jty_file_handle=&file_token;
static const unsigned char input[]={2,0,45,0,1,9,0,128};
static unsigned reads,heap_calls;
static int seek_bad,read_short,heap_bad;
static size_t position;
static long file_length=sizeof(input);
static long LbFileLengthHandle(void* file) {(void)file;return file_length;}
static int LbFileSeek(void* file,long offset,unsigned char origin) {(void)file;(void)origin;position=(size_t)offset;return seek_bad?-1:0;}
static int LbFileRead(void* file,void* target,unsigned long length) {
    (void)file;++reads;if(read_short && length)--length;
    REQUIRE(position+length<=sizeof(input));memcpy(target,input+position,length);return (int)length;
}
static void* he_alloc(size_t size) {++heap_calls;return heap_bad?NULL:malloc(size);}
static uint8_t* rgb_to_pal_table;
static void load_rgb_to_pal_table(void) {}
'''
tests=r'''
static void Clear(void) {
    free(sprite_heap_handle[0]);sprite_heap_handle[0]=NULL;keepsprite[0]=NULL;
    entries[0].DataOffset=0;entries[1].DataOffset=4;creature_table_length=4;
    seek_bad=read_short=heap_bad=0;file_length=sizeof(input);
}
int main(void) {
    size_t bytes=99;Clear();
    REQUIRE(load_single_frame(&sprite_heap_handle[0],0));
    REQUIRE(heap_calls==1 && reads==1);
    REQUIRE(get_keepersprite_source_extent(0,sprite_heap_handle[0],&bytes) && bytes==4);
    REQUIRE(memcmp(sprite_heap_handle[0],input,4)==0);
    entries[0].DataOffset=7;entries[1].DataOffset=1;
    REQUIRE(get_keepersprite_source_extent(0,sprite_heap_handle[0],&bytes) && bytes==4);
    REQUIRE(get_keepersprite_source_extent(-1,sprite_heap_handle[0],&bytes) && bytes==4);
    REQUIRE(!get_keepersprite_source_extent(0,input,&bytes) && bytes==0);
    keepsprite[0]=NULL;
    REQUIRE(!get_keepersprite_source_extent(0,sprite_heap_handle[0],&bytes) && bytes==0);
    Clear();unsigned before=heap_calls;
    read_short=1;REQUIRE(!load_single_frame(&sprite_heap_handle[0],0));
    REQUIRE(!sprite_heap_handle[0] && !keepsprite[0] && heap_calls==before);
    read_short=0;seek_bad=1;REQUIRE(!load_single_frame(&sprite_heap_handle[0],0));
    seek_bad=0;heap_bad=1;REQUIRE(!load_single_frame(&sprite_heap_handle[0],0));
    REQUIRE(!sprite_heap_handle[0] && !keepsprite[0]);Clear();
    creature_table=NULL;REQUIRE(!load_single_frame(&sprite_heap_handle[0],0));creature_table=entries;
    creature_table_length=1;REQUIRE(!load_single_frame(&sprite_heap_handle[0],0));Clear();
    entries[1].DataOffset=UINT32_MAX;REQUIRE(!load_single_frame(&sprite_heap_handle[0],0));Clear();
    entries[0].DataOffset=5;entries[1].DataOffset=4;REQUIRE(!load_single_frame(&sprite_heap_handle[0],0));Clear();
    entries[1].DataOffset=0;REQUIRE(!load_single_frame(&sprite_heap_handle[0],0));
    REQUIRE(!sprite_heap_handle[0] && !keepsprite[0]);Clear();
    file_length=3;REQUIRE(!load_single_frame(&sprite_heap_handle[0],0));Clear();
    entries[1].DataOffset=3;REQUIRE(load_single_frame(&sprite_heap_handle[0],0));
    REQUIRE(get_keepersprite_source_extent(0,sprite_heap_handle[0],&bytes) && bytes==3);Clear();
    REQUIRE(!get_keepersprite_source_extent(0,input,NULL));
    REQUIRE(!get_keepersprite_source_extent(KEEPERSPRITE_ADD_OFFSET+KEEPERSPRITE_ADD_NUM,input,&bytes));
    keepersprite_add[2]=(unsigned char*)input;keepersprite_add_bytes[2]=sizeof(input);
    REQUIRE(get_keepersprite_source_extent(KEEPERSPRITE_ADD_OFFSET+2,input,&bytes) && bytes==sizeof(input));
    REQUIRE(get_keepersprite_source_extent(-1,input,&bytes) && bytes==sizeof(input));
    keepersprite_add[2]=NULL;REQUIRE(!get_keepersprite_source_extent(-1,input,&bytes));
    REQUIRE(sprite_rle_capacity(SIZE_MAX,1)==0 && sprite_rle_capacity(1,SIZE_MAX)==0);
    REQUIRE(sprite_rle_capacity(0,1)==0 && sprite_rle_capacity(1,0)==0);
    const unsigned w=254,h=254;const size_t capacity=sprite_rle_capacity(w,h);
    uint32_t* rgba=malloc((size_t)w*h*4);uint8_t* guard=malloc(capacity+32);
    uint8_t* table=malloc(64*64*64);REQUIRE(rgba && guard && table);
    memset(table,17,64*64*64);memset(guard,0xa6,capacity+32);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)rgba[y*w+x]=((x+y)&1)?UINT32_MAX:0;
    struct TbHugeSprite image={guard+16,NULL,w,h};
    const size_t encoded=compress_raw(&image,(unsigned char*)rgba,0,0,w,h,table,capacity);
    REQUIRE(encoded> (w+2)*(h+3) && encoded<=capacity);
    uint8_t* legacy_bytes=malloc(capacity);REQUIRE(legacy_bytes);
    memset(legacy_bytes,0xa6,capacity);
    struct TbHugeSprite legacy_image={legacy_bytes,NULL,w,h};
    legacy_compress_raw(&legacy_image,(unsigned char*)rgba,0,0,w,h,table);
    REQUIRE(memcmp(image.Data,legacy_bytes,capacity)==0);
    free(legacy_bytes);
    size_t pos=0;
    for(unsigned y=0;y<h;++y) {
        unsigned x=0;
        for(;;) {
            REQUIRE(pos<encoded);const unsigned cmd=image.Data[pos++];if(!cmd)break;
            if(cmd>=128){unsigned skip=256-cmd;while(skip--){REQUIRE(x<w && !((x+y)&1));++x;}}
            else for(unsigned n=0;n<cmd;++n){REQUIRE(x<w && ((x+y)&1) && pos<encoded && image.Data[pos++]==17);++x;}
        }
        // The encoder intentionally omits a transparent row tail.
        while(x<w){REQUIRE(!((x+y)&1));++x;}
    }
    REQUIRE(pos==encoded);
    for(unsigned i=0;i<16;++i)REQUIRE(guard[i]==0xa6 && guard[capacity+16+i]==0xa6);
    memset(guard,0xa6,capacity+32);
    REQUIRE(!compress_raw(&image,(unsigned char*)rgba,0,0,w,h,table,capacity-1));
    REQUIRE(!compress_raw(&image,(unsigned char*)rgba,1,0,w,h,table,capacity));
    REQUIRE(!compress_raw(&image,(unsigned char*)rgba,0,0,w,h,NULL,capacity));
    for(size_t i=0;i<capacity+32;++i)REQUIRE(guard[i]==0xa6);
    // Same exact old encoder bytes for opaque, transparent, alternating and
    // pseudo-random alpha at command-split boundaries (127/128 pixels).
    for(unsigned width=1;width<=254;++width) {
        image.SWidth=width;image.SHeight=17;
        const size_t cap=sprite_rle_capacity(width,17);
        uint8_t* legacy_data=malloc(cap);REQUIRE(legacy_data);
        struct TbHugeSprite reference={legacy_data,NULL,width,17};
        for(unsigned pattern=0;pattern<4;++pattern) {
            for(unsigned i=0;i<width*17;++i)
                rgba[i]=(pattern==0 || (pattern==2 && (i&1)) || (pattern==3 && ((i*37+11)%7)<4))?UINT32_MAX:0;
            memset(image.Data,0xa6,cap);memset(legacy_data,0xa6,cap);
            const size_t used=compress_raw(&image,(unsigned char*)rgba,0,0,width,17,table,cap);
            REQUIRE(used && used<=cap);
            legacy_compress_raw(&reference,(unsigned char*)rgba,0,0,width,17,table);
            REQUIRE(memcmp(image.Data,legacy_data,cap)==0);
        }
        free(legacy_data);
    }
    free(table);free(guard);free(rgba);
    puts("{\"sourceExtentLoader\":\"passed\",\"customCheckerboard\":\"passed\",\"fixture\":\"exact production function bodies with isolated dependencies\"}");
    return 0;
}
'''
source=shim+'\n'.join(functions)+tests
fixture=out/'source-fixture.c';fixture.write_text(source)
exe=out/('source-fixture.exe' if 'mingw' in a.compiler else 'source-fixture')
cmd=[a.compiler,'-std=c11','-O1','-Wall','-Wextra','-Werror',str(fixture),'-o',str(exe)]
if 'mingw' not in a.compiler: cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
subprocess.run(cmd,check=True)
result={'compilerCommand':cmd,'fixtureSha256':hashlib.sha256(source.encode()).hexdigest(),'productionFunctionSha256':[hashlib.sha256(f.encode()).hexdigest() for f in functions]}
if 'mingw' not in a.compiler:
    run=subprocess.run([str(exe)],capture_output=True,text=True,check=True);result['result']=json.loads(run.stdout)
(out/'source-fixture-evidence.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result))
