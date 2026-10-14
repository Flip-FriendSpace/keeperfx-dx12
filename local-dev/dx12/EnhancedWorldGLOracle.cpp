// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include <glad/glad.h>
#include "EnhancedWorldGLOracle.h"
#include "WorldOverlayPass.h"
#include "kfx/renderer/opengl/GLShaders.h"
#include <algorithm>
#include <cstddef>
#include <stdexcept>

namespace kfx::render::dx12 {
namespace {
void Require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
struct StageObjects {
    GLuint programs[2]{},textures[5]{},vaos[2]{},buffers[2]{};
    ~StageObjects(){
        glUseProgram(0);glBindVertexArray(0);
        for(auto p:programs)if(p)glDeleteProgram(p);
        glDeleteTextures(5,textures);glDeleteVertexArrays(2,vaos);glDeleteBuffers(2,buffers);
    }
};
GLuint Compile(GLenum kind,const char* text){
    const auto shader=glCreateShader(kind);Require(shader!=0,"Create enhanced reference shader");
    glShaderSource(shader,1,&text,nullptr);glCompileShader(shader);
    GLint ok=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
    if(!ok){std::array<char,8192> log{};glGetShaderInfoLog(shader,GLsizei(log.size()),nullptr,log.data());
        glDeleteShader(shader);throw std::runtime_error(std::string("Compile enhanced production shader: ")+log.data());}
    return shader;
}
void Program(GLuint& program,const char* vs,const char* ps){
    GLuint vertex=Compile(GL_VERTEX_SHADER,vs),pixel=0;
    try{pixel=Compile(GL_FRAGMENT_SHADER,ps);program=glCreateProgram();Require(program!=0,"Create enhanced reference program");
        glAttachShader(program,vertex);glAttachShader(program,pixel);glLinkProgram(program);
        GLint ok=0;glGetProgramiv(program,GL_LINK_STATUS,&ok);Require(ok!=0,"Link enhanced reference program");
    }catch(...){glDeleteShader(vertex);if(pixel)glDeleteShader(pixel);throw;}
    glDeleteShader(vertex);glDeleteShader(pixel);
}
void Texture(GLenum target,GLuint texture){
    glBindTexture(target,texture);glTexParameteri(target,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(target,GL_TEXTURE_MAG_FILTER,GL_NEAREST);glTexParameteri(target,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(target,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
}
void Attribute(GLuint slot,GLint count,GLsizei stride,size_t offset){
    glEnableVertexAttribArray(slot);glVertexAttribPointer(slot,count,GL_FLOAT,GL_FALSE,stride,reinterpret_cast<void*>(offset));
}
}
void DrawEnhancedWorldGLStages(const EnhancedWorldPass& world,const Image& atlas)
{
    StageObjects owned;
    Program(owned.programs[0],WORLD_VERTEX_SHADER,WORLD_FRAGMENT_SHADER);
    Program(owned.programs[1],SHADOW_VERTEX_SHADER,SHADOW_FRAGMENT_SHADER);
    glGenTextures(5,owned.textures);glGenVertexArrays(2,owned.vaos);glGenBuffers(2,owned.buffers);
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    const uint32_t layers=uint32_t(std::max<size_t>(1,(world.tiles.size()+2047)/2048));
    std::vector<uint8_t> tiles(size_t(2048)*1024*layers);
    for(size_t tile=0;tile<world.tiles.size();++tile)for(size_t y=0;y<32;++y)
        std::copy_n(world.tiles[tile].data()+y*32,32,tiles.data()+(tile/2048)*2048*1024
            +(tile%2048/64*32+y)*2048+(tile%64)*32);
    glActiveTexture(GL_TEXTURE0);Texture(GL_TEXTURE_2D_ARRAY,owned.textures[0]);
    glTexImage3D(GL_TEXTURE_2D_ARRAY,0,GL_R8,2048,1024,layers,0,GL_RED,GL_UNSIGNED_BYTE,tiles.data());
    glActiveTexture(GL_TEXTURE1);Texture(GL_TEXTURE_2D,owned.textures[1]);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,256,1,0,GL_RGBA,GL_UNSIGNED_BYTE,world.palette_rgba.data());
    glActiveTexture(GL_TEXTURE2);Texture(GL_TEXTURE_2D,owned.textures[2]);
    glTexImage2D(GL_TEXTURE_2D,0,GL_R16UI,511,511,0,GL_RED_INTEGER,GL_UNSIGNED_SHORT,world.lightmap.data());
    glActiveTexture(GL_TEXTURE3);Texture(GL_TEXTURE_2D,owned.textures[3]);
    glTexImage2D(GL_TEXTURE_2D,0,GL_R8,256,64,0,GL_RED,GL_UNSIGNED_BYTE,world.fade.data());
    const auto program=owned.programs[0];glUseProgram(program);
    glUniform1i(glGetUniformLocation(program,"u_tile_atlas"),0);
    glUniform1i(glGetUniformLocation(program,"u_palette"),1);
    glUniform1i(glGetUniformLocation(program,"u_lightmap"),2);
    glUniform1i(glGetUniformLocation(program,"u_fade_table"),3);
    const auto& s=world.shade_settings;
    glUniform1f(glGetUniformLocation(program,"u_fullbright"),s[0]);
    glUniform1f(glGetUniformLocation(program,"u_ambient"),s[1]);
    glUniform1f(glGetUniformLocation(program,"u_shade_scale"),s[2]);
    glUniform1f(glGetUniformLocation(program,"u_shade_gamma"),s[3]);
    glUniform1i(glGetUniformLocation(program,"u_lighting_mode"),GLint(s[4]));
    glUniform1i(glGetUniformLocation(program,"u_darkness_mode"),GLint(s[5]));
    glUniform1f(glGetUniformLocation(program,"u_fog_speed"),s[6]);
    glUniform1f(glGetUniformLocation(program,"u_fog_density"),s[7]);
    glUniform1i(glGetUniformLocation(program,"u_tile_filter"),GLint(s[8]));
    glUniform1f(glGetUniformLocation(program,"u_time"),world.time);
    struct Vertex {float x,y,z,u,v,shade,sx,sy,w,layer,wx,wy,wz,tile;};
    std::vector<Vertex> vertices;vertices.reserve(world.terrain.size()*3);
    for(const auto& triangle:world.terrain)for(const auto& v:triangle.vertices){
        const uint32_t tile=triangle.tile==MissingEnhancedTile?0:triangle.tile;
        vertices.push_back({v.position.x,v.position.y,v.position.z,v.u,v.v,v.shade,v.stl_x,v.stl_y,
            v.position.w,float(tile/2048),0,0,0,float(tile%2048)});
    }
    glBindVertexArray(owned.vaos[0]);glBindBuffer(GL_ARRAY_BUFFER,owned.buffers[0]);
    glBufferData(GL_ARRAY_BUFFER,GLsizeiptr(vertices.size()*sizeof(Vertex)),vertices.data(),GL_STREAM_DRAW);
    Attribute(0,3,sizeof(Vertex),offsetof(Vertex,x));Attribute(1,2,sizeof(Vertex),offsetof(Vertex,u));
    Attribute(2,1,sizeof(Vertex),offsetof(Vertex,shade));Attribute(3,2,sizeof(Vertex),offsetof(Vertex,sx));
    Attribute(4,1,sizeof(Vertex),offsetof(Vertex,w));Attribute(5,1,sizeof(Vertex),offsetof(Vertex,layer));
    Attribute(6,3,sizeof(Vertex),offsetof(Vertex,wx));Attribute(7,1,sizeof(Vertex),offsetof(Vertex,tile));
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glDisable(GL_BLEND);glDepthMask(GL_TRUE);glDepthFunc(GL_ALWAYS);
    for(size_t i=0;i<world.terrain.size();++i){
        glUniform1f(glGetUniformLocation(program,"u_missing_tile"),world.terrain[i].tile==MissingEnhancedTile?1.0f:0.0f);
        glDrawArrays(GL_TRIANGLES,GLint(i*3),3);
    }
    const auto shadow_program=owned.programs[1];glUseProgram(shadow_program);
    glUniform2f(glGetUniformLocation(shadow_program,"u_viewport"),float(world.viewport_width),float(world.viewport_height));
    glUniform1i(glGetUniformLocation(shadow_program,"u_sprite"),0);
    glActiveTexture(GL_TEXTURE0);Texture(GL_TEXTURE_2D_ARRAY,owned.textures[4]);
    glTexImage3D(GL_TEXTURE_2D_ARRAY,0,GL_RG8,256,256,1,0,GL_RG,GL_UNSIGNED_BYTE,nullptr);
    struct ShadowVertex {float x,y,u,v,layer,z,darken;};
    glBindVertexArray(owned.vaos[1]);glBindBuffer(GL_ARRAY_BUFFER,owned.buffers[1]);
    Attribute(0,2,sizeof(ShadowVertex),offsetof(ShadowVertex,x));Attribute(1,2,sizeof(ShadowVertex),offsetof(ShadowVertex,u));
    Attribute(2,1,sizeof(ShadowVertex),offsetof(ShadowVertex,layer));Attribute(3,1,sizeof(ShadowVertex),offsetof(ShadowVertex,z));
    Attribute(4,1,sizeof(ShadowVertex),offsetof(ShadowVertex,darken));
    glEnable(GL_BLEND);glBlendEquation(GL_FUNC_ADD);glBlendFunc(GL_ZERO,GL_SRC_COLOR);
    glDepthMask(GL_FALSE);glDepthFunc(GL_LEQUAL);
    std::array<uint8_t,256*256*2> coverage{};
    for(const auto& shadow:world.shadows){
        coverage.fill(0);
        for(uint32_t y=0;y<shadow.source_height;++y)for(uint32_t x=0;x<shadow.source_width;++x)
            coverage[(y*256+x)*2+1]=atlas.rgba[(size_t(y+shadow.mask_y)*atlas.width+x+shadow.mask_x)*4+1];
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY,0,0,0,0,256,256,1,GL_RG,GL_UNSIGNED_BYTE,coverage.data());
        std::array<ShadowVertex,6> quad{};constexpr int corners[]={0,1,2,0,2,3};
        for(size_t i=0;i<quad.size();++i){const auto& v=shadow.vertices[corners[i]];quad[i]={v.x,v.y,v.u,v.v,0,shadow.z_ndc,shadow.darken};}
        glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad.data(),GL_STREAM_DRAW);glDrawArrays(GL_TRIANGLES,0,6);
    }
    Require(glGetError()==GL_NO_ERROR,"Enhanced production GL terrain/shadow stage failed");
}
void ValidateEnhancedCompositionGLInputs(const EnhancedWorldPass& world,uint64_t atlas_bytes,size_t clut_count,
    const SceneGLCompositionTrace& composition)
{
    const auto require=[](bool value,const char* reason){if(!value)throw std::invalid_argument(reason);};
    const auto& overlays=composition.overlays;const auto& lens=composition.lens;const auto& pass=lens.pass;
    require(composition.present&&composition.complete&&composition.completed_phases==15,
        "Incomplete enhanced GL composition");
    ValidateSceneGLOverlayPass(overlays);
    require(overlays.native_width==world.native_width&&overlays.native_height==world.native_height,
        "Composed GL native target mismatch");
    for(float c:composition.screen_clear_rgba)require(std::isfinite(c)&&c>=0&&c<=1,"Invalid composed screen clear");
    require(composition.screen_clear_rgba[3]==1,"Invalid composed screen clear alpha");
    std::array<uint32_t,3> counts{};
    for(const auto& q:overlays.draws)++counts[unsigned(q.phase)];
    require(counts==composition.phase_quad_counts,"Incomplete composed GL overlay phase counts");
    uint64_t bytes=atlas_bytes+uint64_t(clut_count)*1024+uint64_t(world.tiles.size())*1024
        +uint64_t(world.lightmap.size())*2+world.palette_rgba.size()+world.fade.size()
        +uint64_t(overlays.colour_rows.size())*1024+overlays.fade.size();
    for(const auto& source:overlays.sources)bytes+=source.indices.size()+source.initialized_rows.size();
    bytes+=uint64_t(pass.remap.size())*4+pass.overlay.size()+pass.density.size()+pass.palette_rgba.size()
        +pass.fade.size()+pass.rgb6_lookup.size();
    require(bytes<=SceneGLCompositionOwnedByteLimit,"Composed GL resources exceed the owned composition budget");
    require(lens.operation<=SceneGLLensOperation::NotDrawn&&lens.requested_type<=4,"Invalid composed lens branch");
    if(!lens.capture_began)require(composition.screen_clear_rgba==world.clear_rgba,"Screen world and composition clears differ");
    if(!lens.requested_active){require(lens==SceneGLLensTrace{},"Inactive composed lens carries state");return;}
    require(lens.operation!=SceneGLLensOperation::None,"Active composed lens has no resolution result");
    if(lens.operation==SceneGLLensOperation::NotDrawn){
        require(!lens.fallback_copy&&pass==SceneGLLensPass{}&&lens.resources==std::array<SceneGLUploadStamp,6>{},
            "Undrawn composed lens carries resources");return;
    }
    require(lens.capture_began&&unsigned(pass.kind)==unsigned(lens.operation)-1
        &&lens.fallback_copy==(lens.operation==SceneGLLensOperation::Copy&&lens.requested_type!=0),
        "Composed lens branch and operation disagree");
    require((lens.operation!=SceneGLLensOperation::Remap||lens.requested_type==2||lens.requested_type==3)
        &&(lens.operation!=SceneGLLensOperation::Mist||lens.requested_type==1)
        &&(lens.operation!=SceneGLLensOperation::Overlay||lens.requested_type==4),"Composed lens request and operation disagree");
    ValidateSceneGLLensPass(pass);
    require(pass.source_width==world.native_width&&pass.source_height==world.native_height
        &&pass.target_width==world.native_width&&pass.target_height==world.native_height&&pass.source_x==pass.destination_x,
        "Composed GL lens native source or target mismatch");
    const bool mist=lens.operation==SceneGLLensOperation::Mist,overlay=lens.operation==SceneGLLensOperation::Overlay;
    const std::array<bool,6> used{mist||overlay,mist,mist||overlay,mist,lens.operation==SceneGLLensOperation::Remap,overlay};
    for(size_t i=0;i<used.size();++i)require(used[i]?(lens.resources[i].generation&&lens.resources[i].revision)
        :lens.resources[i]==SceneGLUploadStamp{},"Invalid composed lens upload provenance");
}
namespace {
struct CompositionObjects {
    std::array<GLuint,6> programs{};
    std::array<GLuint,7> textures{}; // UI row/fade, screen colour, lens auxiliary/palette/RGB6/fade.
    std::vector<GLuint> sources;
    std::array<GLuint,2> buffers{},vaos{};
    GLuint screen_fbo=0;
    ~CompositionObjects(){
        glUseProgram(0);glBindVertexArray(0);glBindFramebuffer(GL_FRAMEBUFFER,0);
        for(auto p:programs)if(p)glDeleteProgram(p);
        glDeleteTextures(GLsizei(textures.size()),textures.data());
        if(!sources.empty())glDeleteTextures(GLsizei(sources.size()),sources.data());
        glDeleteBuffers(GLsizei(buffers.size()),buffers.data());glDeleteVertexArrays(GLsizei(vaos.size()),vaos.data());
        if(screen_fbo)glDeleteFramebuffers(1,&screen_fbo);
    }
};
void CompositionTexture(GLenum unit,GLuint texture,GLint internal,uint32_t width,uint32_t height,
    GLenum format,GLenum type,const void* pixels)
{
    glActiveTexture(unit);Texture(GL_TEXTURE_2D,texture);
    glTexImage2D(GL_TEXTURE_2D,0,internal,width,height,0,format,type,pixels);
}
void DrawCompositionOverlays(const SceneGLOverlayPass& pass,CompositionObjects& objects)
{
    glViewport(0,0,pass.native_width,pass.native_height);
    if(pass.draws.empty())return;
    constexpr const char* fragments[]={UI_SPRITE_FRAGMENT_SHADER,UI_SPRITE_COLORED_FRAGMENT_SHADER,
        UI_SOLID_FRAGMENT_SHADER,UI_REMAP_FRAGMENT_SHADER,UI_CLUT_FRAGMENT_SHADER};
    for(unsigned i=0;i<5;++i)Program(objects.programs[i],UI_VERTEX_SHADER,fragments[i]);
    objects.sources.resize(pass.sources.size());
    if(!objects.sources.empty())glGenTextures(GLsizei(objects.sources.size()),objects.sources.data());
    for(size_t i=0;i<pass.sources.size();++i){const auto& source=pass.sources[i];
        CompositionTexture(GL_TEXTURE0,objects.sources[i],GL_R8,source.width,source.height,GL_RED,GL_UNSIGNED_BYTE,source.indices.data());}
    if(!pass.fade.empty())CompositionTexture(GL_TEXTURE2,objects.textures[1],GL_R8,256,64,GL_RED,GL_UNSIGNED_BYTE,pass.fade.data());
    glEnable(GL_BLEND);glBlendEquation(GL_FUNC_ADD);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glDepthMask(GL_FALSE);
    glBindVertexArray(objects.vaos[0]);glBindBuffer(GL_ARRAY_BUFFER,objects.buffers[0]);
    Attribute(0,2,sizeof(WorldOverlayVertex),0);Attribute(1,2,sizeof(WorldOverlayVertex),8);
    Attribute(2,4,sizeof(WorldOverlayVertex),16);Attribute(3,1,sizeof(WorldOverlayVertex),32);
    for(const auto& q:pass.draws){
        if(q.depth_test){glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LEQUAL);}else glDisable(GL_DEPTH_TEST);
        if(q.scissor_enabled){glEnable(GL_SCISSOR_TEST);glScissor(q.scissor_x,int(pass.native_height)-q.scissor_y-q.scissor_h,q.scissor_w,q.scissor_h);}
        else glDisable(GL_SCISSOR_TEST);
        const auto program=objects.programs[unsigned(q.mode)];glUseProgram(program);
        glUniform2f(glGetUniformLocation(program,"u_screen_size"),float(pass.native_width),float(pass.native_height));
        glUniform1i(glGetUniformLocation(program,"u_sprite_atlas"),0);
        if(q.source!=NoSceneGLOverlayResource){glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,objects.sources[q.source]);}
        if(q.colour_row!=NoSceneGLOverlayResource){
            CompositionTexture(GL_TEXTURE1,objects.textures[0],GL_RGBA8,256,1,GL_RGBA,GL_UNSIGNED_BYTE,pass.colour_rows[q.colour_row].data());
            glUniform1i(glGetUniformLocation(program,"u_palette"),1);glUniform1i(glGetUniformLocation(program,"u_clut"),1);
            glUniform1f(glGetUniformLocation(program,"u_clut_v"),0.5f);
        }
        glUniform1i(glGetUniformLocation(program,"u_fade_table"),2);glUniform1f(glGetUniformLocation(program,"u_remap_row"),q.fade_row);
        const auto v=[&](float x,float y,float u,float w){return WorldOverlayVertex{x,y,u,w,q.colour,q.z_ndc};};
        const WorldOverlayVertex vertices[]={v(q.x0,q.y0,q.u0,q.v0),v(q.x1,q.y0,q.u1,q.v0),v(q.x1,q.y1,q.u1,q.v1),
            v(q.x0,q.y0,q.u0,q.v0),v(q.x1,q.y1,q.u1,q.v1),v(q.x0,q.y1,q.u0,q.v1)};
        glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_STREAM_DRAW);glDrawArrays(GL_TRIANGLES,0,6);
        Require(glGetError()==GL_NO_ERROR,"Composed production GL overlay draw failed");
    }
}
void DrawCompositionLens(const SceneGLLensPass& pass,GLuint source,CompositionObjects& objects)
{
    const char* fragment=LENS_COPY_FRAGMENT_SHADER;
    if(pass.kind==SceneGLLensPassKind::Mist)fragment=LENS_MIST_FRAGMENT_SHADER;
    else if(pass.kind==SceneGLLensPassKind::Remap)fragment=LENS_REMAP_FRAGMENT_SHADER;
    else if(pass.kind==SceneGLLensPassKind::Overlay)fragment=LENS_OVERLAY_FRAGMENT_SHADER;
    Program(objects.programs[5],LENS_COMPOSITE_VERTEX_SHADER,fragment);
    if(pass.kind==SceneGLLensPassKind::Mist)
        CompositionTexture(GL_TEXTURE1,objects.textures[3],GL_R8,256,256,GL_RED,GL_UNSIGNED_BYTE,pass.density.data());
    else if(pass.kind==SceneGLLensPassKind::Remap)
        CompositionTexture(GL_TEXTURE1,objects.textures[3],GL_RG16UI,pass.remap_width,pass.remap_height,GL_RG_INTEGER,GL_UNSIGNED_SHORT,pass.remap.data());
    else if(pass.kind==SceneGLLensPassKind::Overlay)
        CompositionTexture(GL_TEXTURE1,objects.textures[3],GL_R8,pass.overlay_width,pass.overlay_height,GL_RED,GL_UNSIGNED_BYTE,pass.overlay.data());
    if(!pass.palette_rgba.empty()){
        CompositionTexture(GL_TEXTURE2,objects.textures[4],GL_RGBA8,256,1,GL_RGBA,GL_UNSIGNED_BYTE,pass.palette_rgba.data());
        CompositionTexture(GL_TEXTURE3,objects.textures[5],GL_R8,512,512,GL_RED,GL_UNSIGNED_BYTE,pass.rgb6_lookup.data());
    }
    if(!pass.fade.empty())CompositionTexture(GL_TEXTURE4,objects.textures[6],GL_R8,256,64,GL_RED,GL_UNSIGNED_BYTE,pass.fade.data());
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,source);
    const auto program=objects.programs[5];glUseProgram(program);
    const auto location=[&](const char* name){return glGetUniformLocation(program,name);};
    glUniform1i(location("u_scene"),0);glUniform1i(location("u_mist"),1);glUniform1i(location("u_remap"),1);glUniform1i(location("u_overlay"),1);
    glUniform1i(location("u_palette"),2);glUniform1i(location("u_index_lookup"),3);glUniform1i(location("u_fade_table"),4);
    glUniform1f(location("u_src_x"),float(pass.source_x));glUniform2f(location("u_view_size"),float(pass.viewport_width),float(pass.viewport_height));
    glUniform2f(location("u_pos"),pass.mist_offsets[0],pass.mist_offsets[1]);glUniform2f(location("u_sec"),pass.mist_offsets[2],pass.mist_offsets[3]);
    glUniform1f(location("u_lightness"),float(pass.mist_lightness));glUniform1i(location("u_alpha"),pass.overlay_alpha);
    constexpr float quad[]={-1,-1,0,0,1,-1,1,0,1,1,1,1,-1,-1,0,0,1,1,1,1,-1,1,0,1};
    glBindVertexArray(objects.vaos[1]);glBindBuffer(GL_ARRAY_BUFFER,objects.buffers[1]);glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STREAM_DRAW);
    Attribute(0,2,4*sizeof(float),0);Attribute(1,2,4*sizeof(float),2*sizeof(float));
    const GLint bottom=GLint(pass.target_height)-pass.destination_y-GLint(pass.viewport_height);
    glViewport(pass.destination_x,bottom,pass.viewport_width,pass.viewport_height);
    glEnable(GL_SCISSOR_TEST);glScissor(pass.destination_x,bottom,pass.viewport_width,pass.viewport_height);
    glDisable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);glDisable(GL_BLEND);glDrawArrays(GL_TRIANGLES,0,6);
    Require(glGetError()==GL_NO_ERROR,"Composed production GL lens draw failed");
}
}
Image FinishEnhancedCompositionGLReference(const SceneGLCompositionTrace& composition,unsigned native_colour_texture)
{
    // RenderSpritesGL owns the current context and native RGBA8+D24 surface.
    // Nothing is read back until every overlay and effective lens has executed.
    CompositionObjects objects;glGenTextures(GLsizei(objects.textures.size()),objects.textures.data());
    glGenBuffers(GLsizei(objects.buffers.size()),objects.buffers.data());glGenVertexArrays(GLsizei(objects.vaos.size()),objects.vaos.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);glPixelStorei(GL_PACK_ALIGNMENT,1);
    glDisable(GL_STENCIL_TEST);glDisable(GL_CULL_FACE);glDisable(GL_MULTISAMPLE);glDisable(GL_FRAMEBUFFER_SRGB);
    glEnable(GL_DITHER);glDepthRange(0,1);
    DrawCompositionOverlays(composition.overlays,objects);
    const auto width=composition.overlays.native_width,height=composition.overlays.native_height;
    const auto& lens=composition.lens;
    if(lens.capture_began){
        // The real screen was cleared independently before binding the lens
        // source. NotDrawn must return that clear, not the offscreen world.
        CompositionTexture(GL_TEXTURE0,objects.textures[2],GL_RGBA8,width,height,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glGenFramebuffers(1,&objects.screen_fbo);glBindFramebuffer(GL_FRAMEBUFFER,objects.screen_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,objects.textures[2],0);
        Require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"Incomplete composed GL screen target");
        glDrawBuffer(GL_COLOR_ATTACHMENT0);glReadBuffer(GL_COLOR_ATTACHMENT0);
        glViewport(0,0,width,height);glDisable(GL_SCISSOR_TEST);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        const auto& clear=composition.screen_clear_rgba;glClearColor(clear[0],clear[1],clear[2],clear[3]);glClear(GL_COLOR_BUFFER_BIT);
        if(lens.operation!=SceneGLLensOperation::NotDrawn)DrawCompositionLens(lens.pass,native_colour_texture,objects);
    }
    glDisable(GL_SCISSOR_TEST);
    std::vector<uint8_t> pixels(size_t(width)*height*4);glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    Require(glGetError()==GL_NO_ERROR,"Composed production GL native readback failed");
    Image result{width,height,std::vector<uint8_t>(pixels.size())};const size_t stride=size_t(width)*4;
    for(uint32_t y=0;y<height;++y)std::copy_n(pixels.data()+size_t(height-1-y)*stride,stride,result.rgba.data()+size_t(y)*stride);
    return result;
}
}
