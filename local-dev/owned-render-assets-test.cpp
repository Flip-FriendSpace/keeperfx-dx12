// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// KeeperFX renderer performance changes: 2026-10-19.
#include "kfx/renderer/OwnedRenderAssets.h"
#include "kfx/renderer/KeeperSpriteDarkenCache.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
using namespace kfx::render;
static OwnedAssetKey Key(uint64_t id=1,uint64_t rev=1,uint64_t gen=1,uint64_t epoch=1) { return {epoch,7,id,gen,rev}; }
static OwnedAssetView View(std::span<const uint8_t> bytes) { return {{OwnedAssetFormat::R8,uint32_t(bytes.size()),1},bytes,bytes.size()}; }
int main()
{
    const std::array<uint8_t,4> a{1,2,3,4},b{5,6,7,8};
    {
        OwnedRenderAssets c(1);
        std::array<uint8_t,6> padded{1,2,99,99,3,4};
        auto first=c.Publish(Key(),{{OwnedAssetFormat::R8,2,2},padded,4});
        assert(first && first.lease.RowStride()==2 && first.lease.Bytes().size()==4);
        assert(std::equal(a.begin(),a.end(),first.lease.Bytes().begin()));
        padded[0]=42; assert(first.lease.Bytes()[0]==1); padded[0]=1;
        auto repeat=c.Publish(Key(),{{OwnedAssetFormat::R8,2,2},padded,4});
        assert(repeat.status==OwnedAssetStatus::Reused && repeat.lease.Bytes().data()==first.lease.Bytes().data());
        auto next=c.Publish(Key(1,2),{{OwnedAssetFormat::R8,2,2},padded,4});
        assert(next && c.Stats().payload_bytes==4 && c.Stats().live_versions==2);
        padded[0]=42;
        assert(c.Publish(Key(1,2),{{OwnedAssetFormat::R8,2,2},padded,4}).status==OwnedAssetStatus::ContentConflict);
        assert(c.Find(Key(1,2)).Bytes()[0]==1);
        auto changed=c.Publish(Key(1,3),{{OwnedAssetFormat::R8,2,2},padded,4});
        assert(changed && first.lease.Bytes()[0]==1 && changed.lease.Bytes()[0]==42);
        assert(c.Publish(Key(),{{OwnedAssetFormat::R8,2,2},padded,4}).status==OwnedAssetStatus::StaleKey);
        assert(c.Stats().payload_bytes==8 && c.Stats().peak_owned_bytes==8);
        assert(c.Publish(Key(2),{{OwnedAssetFormat::R8,2,2},std::span(padded).first(5),4}).status==OwnedAssetStatus::InvalidSource);
        assert(c.Publish(Key(2),{{static_cast<OwnedAssetFormat>(99),2,2},padded,4}).status==OwnedAssetStatus::InvalidSource);
        assert(c.Publish(Key(2),{{OwnedAssetFormat::RGBA8,UINT32_MAX,UINT32_MAX},padded,SIZE_MAX}).status==OwnedAssetStatus::InvalidSource);
    }
    {
        OwnedRenderAssets c(1,{8,4,2,8,8});
        auto held=c.Publish(Key(),View(a)); auto next=c.Publish(Key(1,2),View(b));
        assert(next && c.Stats().payload_bytes==8);
        auto blocked=c.Publish(Key(1,3),View(a));
        assert(blocked.status==OwnedAssetStatus::WouldBlock && blocked.pressure==OwnedAssetPressure::Bytes);
        assert(c.Find(Key(1,2)).Bytes()[0]==5 && c.Stats().peak_owned_bytes==8);
        held.lease={}; c.Collect(); assert(c.Publish(Key(1,3),View(a)));
        assert(c.Stats().payload_bytes==8); next.lease={}; c.Collect(true);
        assert(c.Stats().payload_bytes==0);
        auto restore=c.Publish(Key(1,3),View(b)); // trusted rehydration after final owner release
        assert(restore.status==OwnedAssetStatus::Rehydrated && c.Stats().rehydrations==1);
        assert(c.Publish(Key(1,3),View(a)).status==OwnedAssetStatus::ContentConflict);
        assert(c.Publish(Key(1,2),View(a)).status==OwnedAssetStatus::StaleKey);
    }
    {
        OwnedAssetLease survivor;
        {
            OwnedRenderAssets c(1,{12,2,1,8,8}); survivor=c.Publish(Key(),View(a)).lease;
            assert(!c.ResetBank(7,1) && c.ResetBank(7,2) && c.Find(Key()).Valid());
            assert(c.Publish(Key(),View(a)).status==OwnedAssetStatus::StaleKey);
            auto newer=c.Publish(Key(1,1,2),View(b)); assert(newer);
            assert(c.Publish(Key(2,1,3),View(a)).status==OwnedAssetStatus::GenerationRequired);
            assert(c.AdvanceEpoch(2) && !c.AdvanceEpoch(2));
            assert(c.Stats().payload_bytes==8 && c.Stats().identities==0);
            assert(c.Publish(Key(),View(a)).status==OwnedAssetStatus::WrongEpoch);
            auto now=c.Publish(Key(1,1,1,2),View(a)); assert(now);
            assert(c.Stats().payload_bytes==12 && c.Find(Key()).Valid());
            newer.lease={}; now.lease={}; c.Collect(true); assert(c.Stats().payload_bytes==4);
        }
        std::thread consumer([owned=std::move(survivor)]() mutable { assert(owned.Bytes()[3]==4); owned={}; }); consumer.join();
    }
    {
        OwnedRenderAssets c(1,{8,1,1,2,2}); auto one=c.Publish(Key(),View(a)); assert(one);
        assert(c.Publish(Key(2),View(a)).pressure==OwnedAssetPressure::Identities);
        auto foreign=Key(); foreign.domain=8; assert(c.Publish(foreign,View(a)).pressure==OwnedAssetPressure::Banks);
        auto two=c.Publish(Key(1,2),View(a)); assert(two);
        assert(c.Publish(Key(1,3),View(a)).pressure==OwnedAssetPressure::Versions && c.Stats().payload_bytes==4);
        one.lease={}; assert(c.Publish(Key(1,3),View(a)));
        OwnedRenderAssets content_cap(1,{12,4,2,8,1}); auto first=content_cap.Publish(Key(),View(a)); assert(first);
        assert(content_cap.Publish(Key(1,2),View(b)).pressure==OwnedAssetPressure::Contents);
        assert(content_cap.Find(Key()).Bytes()[0]==1);
    }
    {
        const std::array<uint8_t,5> rle{2,0,7,0,128};
        OwnedRenderAssets c(1,{48,4,2,8,8}); auto sprite=c.PublishKeeperSprite(Key(),rle,2,1,4,2);
        assert(sprite && sprite.lease.Bytes().size()==32); auto pixels=sprite.lease.Bytes();
        assert(pixels[0]==0 && pixels[1]==255 && pixels[4]==7 && pixels[5]==255);
        for(size_t i=8;i<pixels.size();++i) assert(pixels[i]==0);
        assert(pixels[2]==0 && pixels[3]==0 && pixels[6]==0 && pixels[7]==0);
        assert(c.Stats().temporary_bytes==0 && c.Stats().payload_bytes==32 && c.Stats().peak_owned_bytes==48);
        assert(c.PublishKeeperSprite(Key(1,2),std::span(rle).first(2),2,1,4,2).status==OwnedAssetStatus::InvalidSource);
        assert(c.Find(Key()).Valid());
        auto identical=c.PublishKeeperSprite(Key(1,2),rle,2,1,4,2);
        assert(identical && identical.lease.Bytes().data()==pixels.data());
        OwnedRenderAssets small(1,{47,4,2,8,8});
        assert(small.PublishKeeperSprite(Key(),rle,2,1,4,2).pressure==OwnedAssetPressure::Bytes);
        assert(small.Stats().payload_bytes==0 && small.Stats().temporary_bytes==0 && small.Stats().peak_owned_bytes==16);
    }
    {
        OwnedRenderAssets c(1,{16,16384,8,32768,16});
        for(uint64_t i=0;i<9149;++i) assert(c.Publish(Key(i),View(a)));
        assert(c.Stats().identities==9149 && c.Stats().payload_bytes<=16);
        c.Collect(true); assert(c.Stats().payload_bytes==0 && c.Stats().live_versions==0);
        assert(c.Publish(Key(0),View(a)).status==OwnedAssetStatus::Rehydrated);
        for(uint64_t rev=2;rev<100;++rev) assert(c.Publish(Key(0,rev),View(a)));
        c.Collect(); assert(c.Stats().live_contents==1 && c.Stats().version_records==1);
    }
    {
        const std::array<uint8_t,6> encoded{2,0,7,255,0,128};
        OwnedRenderAssets c(1,{9,4,2,8,8});
        auto ui=c.PublishUISprite(Key(),encoded,3,1);
        assert(ui && ui.lease.Bytes().size()==3 && ui.lease.Bytes()[0]==1 && ui.lease.Bytes()[1]==7 && ui.lease.Bytes()[2]==0);
        assert(c.Stats().peak_owned_bytes==9 && c.Stats().temporary_bytes==0);
        assert(c.PublishUISprite(Key(1,2),std::span(encoded).first(2),3,1).status==OwnedAssetStatus::InvalidSource);
        assert(c.Find(Key()).Valid());
    }
    {
        const std::array<uint8_t,5> bits{0x81,0x80,0xee,0x40,0};
        OwnedRenderAssets c(1,{36,4,2,8,8});
        auto mask=c.PublishBitMask(Key(),bits,3,9,2);
        assert(mask && mask.lease.Bytes().size()==18);
        for(size_t i=0;i<18;++i) assert(mask.lease.Bytes()[i]==((i==0 || i==7 || i==8 || i==10)?255:0));
        assert(c.Stats().peak_owned_bytes==36 && c.Stats().temporary_bytes==0);
        assert(c.PublishBitMask(Key(1),std::span(bits).first(4),3,9,2).status==OwnedAssetStatus::InvalidSource);
        assert(c.PublishBitMask(Key(1),bits,1,9,2).status==OwnedAssetStatus::InvalidSource);
        assert(c.Find(Key()).Valid());
    }
    {
        // Exact rectangular sources preserve every index/coverage byte from the
        // former square allocation, including opaque zero and transparent edges.
        auto encode=[](uint32_t width,uint32_t height) {
            std::vector<uint8_t> bytes;
            for(uint32_t y=0;y<height;++y) {
                bytes.push_back(255); // One transparent pixel.
                for(uint32_t x=1;x<width;) {
                    const auto count=std::min(127u,width-x);bytes.push_back(uint8_t(count));
                    for(uint32_t i=0;i<count;++i)bytes.push_back(uint8_t(x++ + y));
                }
                bytes.push_back(0);
            }
            bytes.push_back(128);return bytes;
        };
        for(const auto& [width,height]:{std::pair{37u,21u},{65u,201u},{257u,68u},{512u,513u},{1024u,17u}}) {
            const auto encoded=encode(width,height);
            const uint32_t dimension=width>512||height>512?1024:width>256||height>256?512:256;
            OwnedRenderAssets assets(1);
            auto padded=assets.PublishKeeperSprite(Key(1),encoded,width,height,dimension,dimension);
            auto tight=assets.PublishKeeperSprite(Key(2),encoded,width,height,width,height);
            assert(padded && tight && tight.lease.Bytes().size()==size_t(width)*height*4);
            for(uint32_t y=0;y<height;++y) {
                const auto a=padded.lease.Bytes().subspan(size_t(y)*dimension*4,size_t(width)*4);
                const auto b=tight.lease.Bytes().subspan(size_t(y)*width*4,size_t(width)*4);
                assert(std::equal(a.begin(),a.end(),b.begin()));
            }
        }
        // Hold two crowded animation frames simultaneously, as front + builder
        // do in live play. The old 256-square sources cannot fit this same budget.
        const auto encoded=encode(65,201);
        OwnedRenderAssets tight(1,{8u*1024u*1024u,256,2,512,512});
        std::vector<OwnedAssetLease> held;
        for(uint64_t i=1;i<=128;++i) {
            auto sprite=tight.PublishKeeperSprite(Key(i),encoded,65,201,65,201);
            assert(sprite);held.push_back(std::move(sprite.lease));
        }
        assert(tight.Stats().payload_bytes==128u*65u*201u*4u);
        assert(tight.Stats().peak_owned_bytes<=8u*1024u*1024u);
        OwnedRenderAssets padded(1,{8u*1024u*1024u,256,2,512,512});held.clear();
        bool blocked=false;
        for(uint64_t i=1;i<=128;++i) {
            auto sprite=padded.PublishKeeperSprite(Key(i),encoded,65,201,256,256);
            if(!sprite){assert(sprite.pressure==OwnedAssetPressure::Bytes);blocked=true;break;}
            held.push_back(std::move(sprite.lease));
        }
        assert(blocked);
    }
    {
        KeeperSpriteDarkenCache cache;
        OwnedRenderAssets assets(1);
        std::array<uint8_t,16> pixels{};
        uint64_t revision=0;
        // Match the original predicate for every palette index and coverage,
        // including repeated hits and different requested source rectangles.
        for(unsigned index=0;index<256;++index) for(const uint8_t coverage:{0,255}) {
            pixels[12]=uint8_t(index);pixels[13]=coverage;
            auto sprite=assets.Publish(Key(1,++revision),{{OwnedAssetFormat::RGBA8,2,2},pixels,8});
            assert(sprite);
            const bool expected=coverage && index>=50 && index<=56;
            assert(cache.Darkens(sprite.lease,2,2)==expected);
            assert(cache.Darkens(sprite.lease,2,2)==expected);
            assert(!cache.Darkens(sprite.lease,1,2) && !cache.Darkens(sprite.lease,2,1));
        }
        pixels[12]=50;pixels[13]=255;
        auto old=assets.Publish(Key(1,++revision),{{OwnedAssetFormat::RGBA8,2,2},pixels,8});assert(old);
        assert(cache.Darkens(old.lease,2,2));
        pixels[13]=0;
        auto collision=assets.Publish(Key(1025),{{OwnedAssetFormat::RGBA8,2,2},pixels,8});assert(collision);
        assert(!cache.Darkens(collision.lease,2,2) && cache.Darkens(old.lease,2,2));
        assert(assets.AdvanceEpoch(2));
        auto current=assets.Publish(Key(1,1,1,2),{{OwnedAssetFormat::RGBA8,2,2},pixels,8});assert(current);
        assert(!cache.Darkens(current.lease,2,2) && cache.Darkens(old.lease,2,2));
        assert(!cache.Darkens({},2,2) && !cache.Darkens(old.lease,0,2));
        assert(!cache.Darkens(old.lease,3,2) && !cache.Darkens(old.lease,2,3));
        old.lease={};collision.lease={};current.lease={};assets.Collect(true);
        assert(assets.Stats().payload_bytes==0);
    }
    {
        // Compact storage preserves all 256 indices, binary coverage, padding
        // and darken classification, including opaque index zero.
        std::vector<uint8_t> encoded;
        for(unsigned y=0;y<3;++y) {
            for(unsigned x=0;x<256;) { const unsigned n=std::min(127u,256-x);
                encoded.push_back(uint8_t(n));for(unsigned k=0;k<n;++k)encoded.push_back(uint8_t(x++)); }
            encoded.push_back(255);encoded.push_back(0);
        }
        encoded.push_back(128);
        OwnedRenderAssets a(1),b(1);
        const auto rgba=a.PublishKeeperSprite(Key(),encoded,257,3,259,5);
        const auto rg=b.PublishKeeperSprite(Key(),encoded,257,3,259,5,OwnedAssetFormat::RG8);
        assert(rgba && rg && rg.lease.RowStride()==518 && rg.lease.Bytes().size()*2==rgba.lease.Bytes().size());
        for(size_t p=0;p<259*5;++p)for(size_t c=0;c<2;++c)assert(rg.lease.Bytes()[p*2+c]==rgba.lease.Bytes()[p*4+c]);
        assert(rg.lease.Bytes()[0]==0 && rg.lease.Bytes()[1]==255);
        KeeperSpriteDarkenCache ca,cb;assert(ca.Darkens(rgba.lease,257,3) && cb.Darkens(rg.lease,257,3));
        assert(b.Stats().peak_owned_bytes==259*5*4 && a.Stats().peak_owned_bytes==259*5*6);
        assert(!b.PublishKeeperSprite(Key(2),encoded,257,3,259,5,OwnedAssetFormat::R8));
        assert(!b.PublishKeeperSprite(Key(2),std::span(encoded).first(2),257,3,259,5,OwnedAssetFormat::RG8));
    }
    {
        OwnedRenderAssets c(1,{16,16,2,16,16});
        for(unsigned id=1;id<=4;++id)assert(c.Publish(Key(id),View(a)));
        auto fifth=c.Publish(Key(5),View(a));assert(fifth && c.Stats().evictions==1 && c.Stats().payload_bytes==16);
        OwnedAssetKey hot{};
        for(unsigned id=1;id<=4;++id)if(c.Find(Key(id)).Valid()){hot=Key(id);break;}
        assert(hot.logical_id);
        auto sixth=c.Publish(Key(6),View(a));assert(sixth && c.Stats().evictions==2 && c.Find(hot).Valid());
        // Pin every remaining publication: admission must fail without eviction.
        std::vector<OwnedAssetLease> pinned;
        for(unsigned id=1;id<=6;++id)if(auto lease=c.Find(Key(id));lease.Valid())pinned.push_back(lease);
        assert(pinned.size()==4);
        assert(!c.Publish(Key(7),View(a)) && c.Stats().evictions==2);
        assert(c.Stats().payload_bytes==16 && c.Stats().peak_owned_bytes==16);
    }
    for(bool versions:{false,true}) {
        OwnedRenderAssets c(1,{1024,16,2,versions?2u:16u,versions?16u:2u});
        assert(c.Publish(Key(1),View(a)));assert(c.Publish(Key(2),View(b)));
        assert(c.Publish(Key(3),View(a)) && c.Stats().evictions==1);
        assert(c.Stats().live_versions==2 && c.Stats().live_contents==2);
    }
    std::cout << "OwnedRenderAssets focused ownership/pressure/reset/rehydration/stride/sprite tests passed\n";
}
