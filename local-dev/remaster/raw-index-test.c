// SPDX-License-Identifier: GPL-2.0-or-later
#include "kfx/renderer/RawIndexSprite.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void roundtrip(size_t w, size_t h)
{
    static uint8_t rgba[1024*4*4], packed[1024*4*3], index[1024*4], mask[1024*4];
    for (size_t i=0;i<w*h;i++)
    {
        rgba[i*4]=rgba[i*4+1]=rgba[i*4+2]=(uint8_t)i;
        rgba[i*4+3]=(i%7==0)?63:(i%7==1?64:255);
    }
    memset(packed,0xA5,sizeof(packed));
    size_t n=kfx_raw_index_rle(packed+1,sizeof(packed)-2,rgba,w,h),pos=0;
    assert(n>0 && packed[0]==0xA5 && packed[n+1]==0xA5);
    memset(index,0,sizeof(index));memset(mask,0,sizeof(mask));
    for(size_t y=0;y<h;y++)
    {
        size_t x=0;
        while(packed[1+pos])
        {
            unsigned c=packed[1+pos++],run=c<128?c:256-c;
            assert(x+run<=w);
            if(c<128)for(unsigned i=0;i<run;i++){index[y*w+x+i]=packed[1+pos++];mask[y*w+x+i]=255;}
            x+=run;
        }
        pos++;
        assert(x==w);
    }
    assert(pos==n);
    for(size_t i=0;i<w*h;i++){assert((mask[i]>0)==(rgba[i*4+3]>=64));if(mask[i])assert(index[i]==rgba[i*4]);}
    uint8_t bad[4]={1,2,1,255},sentinel[3]={0xA5,0xA5,0xA5};
    assert(!kfx_raw_index_rle(sentinel,3,bad,1,1));assert(sentinel[0]==0xA5);
    assert(!kfx_raw_index_rle(packed,1,rgba,w,h));
    assert(!kfx_raw_index_rle(packed,sizeof(packed),rgba,1025,1));
    assert(!kfx_raw_index_rle(NULL,sizeof(packed),rgba,w,h));
}
int main(void){size_t widths[]={1,2,126,127,128,254,255,256,511,1024};for(size_t i=0;i<10;i++)roundtrip(widths[i],4);puts("raw-index roundtrip: all 256 codes, opaque zero, alpha63/64, RLE boundaries and guards passed");return 0;}
