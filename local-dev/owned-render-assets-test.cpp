// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/OwnedRenderAssets.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <thread>
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
    std::cout << "OwnedRenderAssets focused ownership/pressure/reset/rehydration/stride/sprite tests passed\n";
}
