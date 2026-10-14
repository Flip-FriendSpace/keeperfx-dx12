// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/SceneSprites.h"
#include <algorithm>
#include <cassert>
#include <bit>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <limits>
#include <stdexcept>

using namespace kfx::render;
namespace {
void Near(double a, double b, double tolerance=.02) {
    if (std::abs(a-b) > tolerance) throw std::runtime_error("Coordinate mismatch: " + std::to_string(a) + " vs " + std::to_string(b));
}
void Reject(auto f) { bool rejected=false; try { f(); } catch (const std::exception&) { rejected=true; } assert(rejected); }
SceneCapture Fixture()
{
    SceneCapture c; auto& s=c.scene;
    s.epoch=1; s.sequence=2; s.texture_generation=3; s.map_width=s.map_height=20;
    s.camera.viewport_w=1280; s.camera.viewport_h=720; s.camera.scaled_zoom=65536;
    s.camera.position={2560,2560,512}; s.camera.has_rotation=true;
    s.camera.rotation={16384,0,0,0, 0,11585,11585,0, 0,-11585,11585,0};
    s.texture_dimension=32; s.texture_row_stride=32; s.texture_frames.assign(32*1544,0);
    s.top_cubes={0}; s.ceiling_textures={0}; s.cube_materials.resize(1);
    SceneColumn col; col.x=col.y=10; col.cell=210; col.in_view=col.revealed=true; col.light.fill(8192); s.columns.push_back(col);
    SceneObject o; o.id={7,1}; o.position={2688,2688,256}; o.simulation_position=o.position;
    o.size=256; o.draw_class=2; o.animation=5; s.objects.push_back(o);
    c.assets.presentation.push_back({o.id,8192,0,true});
    c.assets.texture_pixels.resize(1024,1);
    for (size_t i=0;i<768;++i) c.assets.display_palette[i]=i%64;
    for (size_t i=0;i<64*256;++i) c.assets.fade[i]=i%256;
    for (unsigned i=0;i<8;++i) {
        SceneSprite sprite; sprite.id=i; sprite.width=4; sprite.height=6; sprite.frame_width=10; sprite.frame_height=12;
        sprite.frame_x=2; sprite.frame_y=3; sprite.pixels.resize(4*6*2,255);
        for (size_t j=0;j<sprite.pixels.size();j+=2) sprite.pixels[j]=uint8_t(i);
        sprite.pixels[1]=0; c.assets.sprites.push_back(sprite);
        c.assets.bindings.push_back({5,0,uint8_t(i),i,i>=5,10,12,-5,-12});
    }
    return c;
}
uint64_t Fingerprint(const SceneSpriteMesh& mesh, bool include_geometry=true)
{
    uint64_t hash=14695981039346656037ull;
    auto add=[&](uint32_t v) {for (int i=0;i<4;++i) {hash^=uint8_t(v>>(i*8));hash*=1099511628211ull;}};
    for (const auto& q:mesh.quads) {
        add(q.id.slot); add(q.direction); add(q.sprite); add(q.flip);
        if (include_geometry) for (const auto& v:q.vertices) for (float f:{v.x,v.y,v.z,v.u,v.v}) add(std::bit_cast<uint32_t>(f));
    }
    for (auto byte:mesh.atlas.rgba) { hash^=byte; hash*=1099511628211ull; }
    return hash;
}
void Check(const SceneCapture& capture, const SceneSpriteMesh& mesh)
{
    CapturedCamera camera(capture.scene.camera);
    for (const auto& q:mesh.quads) {
        auto a=camera.Project(q.vertices[0].x,q.vertices[0].y,q.vertices[0].z);
        auto b=camera.Project(q.vertices[1].x,q.vertices[1].y,q.vertices[1].z);
        auto c=camera.Project(q.vertices[2].x,q.vertices[2].y,q.vertices[2].z);
        assert(a.x < b.x && b.y > c.y); Near(a.y,b.y); Near(b.x,c.x);
        for (const auto& v:q.vertices) assert(std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z)&&v.u>=0&&v.u<=1&&v.v>=0&&v.v<=1);
    }
}
void Tests()
{
    auto c=Fixture(); CapturedCamera camera(c.scene.camera);
    assert(camera.reverse_winding);
    auto p=camera.Project(10,10,2); Near(p.x,640); Near(p.y,360);
    auto v=camera.OffsetOnViewPlane(c.scene.objects[0].position,25,-35);
    auto anchor=camera.Project(10.5,10.5,1); auto shifted=camera.Project(v[0],v[1],v[2]);
    Near(shifted.x-anchor.x,25); Near(shifted.y-anchor.y,-35); Near(shifted.depth,anchor.depth);
    auto upright=camera.OffsetOnSpritePlane(c.scene.objects[0].position,0,-35);
    auto raised=camera.Project(upright[0],upright[1],upright[2]);
    Near(raised.x,anchor.x); Near(raised.y-anchor.y,-35); assert(upright[2]>1 && raised.depth<anchor.depth);
    for (int angle:{0,127,128,1151,1152,1918,1919,1920,2047,-256}) {
        c.scene.objects[0].yaw=angle;
        auto mesh=BuildSceneSprites(c,BuildTerrainMesh(c)); assert(mesh.quads.size()==1); Check(c,mesh);
        assert(mesh.quads[0].direction==((angle+128)&2047)/256);
        assert(mesh.quads[0].flip==((angle&2047)>1151&&(angle&2047)<1919));
        const auto& q=mesh.quads[0]; auto a=camera.Project(q.vertices[0].x,q.vertices[0].y,q.vertices[0].z);
        auto b=camera.Project(q.vertices[2].x,q.vertices[2].y,q.vertices[2].z);
        Near(b.x-a.x,32); Near(a.y-b.y,48); // 4x6 source, scale 8
        const auto left=uint32_t(std::round(std::min(q.vertices[0].u,q.vertices[1].u)*mesh.atlas.width));
        const auto top=uint32_t(std::round(q.vertices[2].v*mesh.atlas.height));
        assert(mesh.atlas.rgba[(size_t(top)*mesh.atlas.width+left)*4+3]==0);
        assert(mesh.atlas.rgba[(size_t(top)*mesh.atlas.width+left+1)*4+3]==255);
    }
    for (int q=0;q<4;++q) {
        c=Fixture(); c.scene.camera.view=SceneView::Front; c.scene.camera.yaw=q*512;
        CapturedCamera front(c.scene.camera); auto center=front.Project(10,10,0); Near(center.x,640); Near(center.y,360);
        auto raised=front.Project(10,10,1); Near(raised.y,344);
        auto mesh=BuildSceneSprites(c,BuildTerrainMesh(c)); assert(mesh.quads.size()==1); Check(c,mesh);
    }
    c=Fixture(); c.scene.camera.view=SceneView::Possession; c.scene.camera.lens_mode=1; c.scene.camera.lens=600;
    c.scene.camera.view_distance=32; c.scene.camera.rotation={16384,0,0,0,0,16384,0,0,0,0,16384,0};
    CapturedCamera perspective(c.scene.camera);
    auto near=perspective.Project(11,8,2), far=perspective.Project(11,6,2);
    Near(near.x-640,2*(far.x-640)); assert(!perspective.Project(10,11,2).in_front);
    c.scene.camera.lens_mode=3; Reject([&]{CapturedCamera invalid(c.scene.camera);});
    c=Fixture(); c.assets.presentation.clear(); Reject([&]{BuildSceneSprites(c,BuildTerrainMesh(c));});
    c=Fixture(); c.assets.presentation[0].id.incarnation++; assert(!ValidateSceneCapture(c));
    c=Fixture(); c.scene.objects[0].render_flags=0x10;
    auto deferred=BuildSceneSprites(c,BuildTerrainMesh(c)); assert(deferred.quads.empty() && deferred.deferred_effects==1);
    c=Fixture(); c.assets.presentation[0].rotates=false; c.scene.camera.yaw=512;
    auto omni=BuildSceneSprites(c,BuildTerrainMesh(c)); assert(omni.quads[0].direction==0 && !omni.quads[0].flip);
    c=Fixture(); c.scene.camera.rotation={}; Reject([&]{CapturedCamera invalid(c.scene.camera);});
    std::cout<<"{\"passed\":true}\n";
}
}
int main(int argc,char** argv)
{
    try {
        if (argc==1) {Tests();return 0;}
        const auto c=LoadSceneCapture(argv[1]);
        const auto comparison=CompareCapturedCamera(*c);
        auto sprites=BuildSceneSprites(*c,BuildTerrainMesh(*c)); Check(*c,sprites);
        auto reversed=*c; std::reverse(reversed.scene.objects.begin(),reversed.scene.objects.end());
        assert(Fingerprint(sprites)==Fingerprint(BuildSceneSprites(reversed,BuildTerrainMesh(reversed))));
        std::cout<<std::setprecision(9)<<"{\"passed\":"<<(comparison.passed?"true":"false")<<",\"checks\":"<<comparison.checked
            <<",\"maxErrorPixels\":"<<comparison.maximum_error<<",\"rmsErrorPixels\":"<<comparison.rms_error
            <<",\"view\":"<<unsigned(c->scene.camera.view)<<",\"lens\":"<<unsigned(c->scene.camera.lens_mode)
            <<",\"viewport\":["<<c->scene.camera.viewport_x<<','<<c->scene.camera.viewport_y<<','<<c->scene.camera.viewport_w<<','<<c->scene.camera.viewport_h<<']'
            <<",\"sprites\":"<<sprites.quads.size()<<",\"clipped\":"<<sprites.clipped<<",\"deferred\":"<<sprites.deferred_effects
            <<",\"contentFingerprint\":\""<<std::hex<<Fingerprint(sprites,false)<<"\",\"vertexComponents\":[";
        bool comma=false;
        for (const auto& q:sprites.quads) for (const auto& v:q.vertices) for (float f:{v.x,v.y,v.z,v.u,v.v}) {
            std::cout<<(comma?",":"")<<f; comma=true;
        }
        std::cout<<"]}\n";
        return comparison.passed?0:1;
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
