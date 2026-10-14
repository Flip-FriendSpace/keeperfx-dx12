// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Owned draw inputs and isolated state around the unchanged production shaders.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <glad/glad.h>
#include "WorldOverlayGLOracle.h"
#include "kfx/renderer/opengl/GLShaders.h"
#include <mutex>

namespace kfx::render::dx12 {
namespace {
std::mutex oracle_mutex; // Invoke GLAD-based diagnostic oracles serially.
thread_local std::string device_identity;
void Require(bool good,const char* reason){if(!good)throw std::runtime_error(reason);}
void CheckGL(const char* reason)
{
    const auto error=glGetError();
    if(error!=GL_NO_ERROR)throw std::runtime_error(std::string(reason)+": GL error "+std::to_string(error));
}
void* LoadGL(const char* name)
{
    void* p=reinterpret_cast<void*>(wglGetProcAddress(name));
    if(!p||p==reinterpret_cast<void*>(1)||p==reinterpret_cast<void*>(2)||p==reinterpret_cast<void*>(3)||p==reinterpret_cast<void*>(-1))
        p=reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("opengl32.dll"),name));
    return p;
}
struct Context {
    HWND window=nullptr;HDC dc=nullptr,previous_dc=nullptr;HGLRC context=nullptr,previous_context=nullptr;
    bool registered=false;
    static constexpr const char* Name="KeeperFXOwnedWorldOverlayOracle";
    Context()
    {
        previous_context=wglGetCurrentContext();previous_dc=wglGetCurrentDC();
        try{
            WNDCLASSA wc{};wc.style=CS_OWNDC;wc.lpfnWndProc=DefWindowProcA;
            wc.hInstance=GetModuleHandleA(nullptr);wc.lpszClassName=Name;
            registered=RegisterClassA(&wc)!=0;
            Require(registered||GetLastError()==ERROR_CLASS_ALREADY_EXISTS,"Register world overlay oracle window");
            window=CreateWindowA(Name,"Owned world overlay reference",WS_OVERLAPPEDWINDOW,0,0,32,32,nullptr,nullptr,wc.hInstance,nullptr);
            Require(window!=nullptr,"Create hidden world overlay oracle window");
            dc=GetDC(window);Require(dc!=nullptr,"Get world overlay oracle DC");
            PIXELFORMATDESCRIPTOR pf{};pf.nSize=sizeof(pf);pf.nVersion=1;
            pf.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;pf.iPixelType=PFD_TYPE_RGBA;
            pf.cColorBits=32;pf.cDepthBits=24;pf.cStencilBits=8;
            const int fmt=ChoosePixelFormat(dc,&pf);
            Require(fmt&&SetPixelFormat(dc,fmt,&pf),"Set world overlay oracle pixel format");
            context=wglCreateContext(dc);Require(context&&wglMakeCurrent(dc,context),"Create world overlay oracle context");
            Require(gladLoadGLLoader(LoadGL)&&GLAD_GL_VERSION_3_3,"World overlay oracle requires OpenGL 3.3");
            const auto string=[](GLenum name){const auto* value=glGetString(name);Require(value!=nullptr,"Missing world overlay GL identity");
                return std::string(reinterpret_cast<const char*>(value));};
            device_identity=string(GL_VENDOR)+"; "+string(GL_RENDERER)+"; "+string(GL_VERSION);
            CheckGL("Initialize world overlay oracle");
        }catch(...){Cleanup();throw;}
    }
    void Cleanup() noexcept
    {
        if(context){wglMakeCurrent(nullptr,nullptr);wglDeleteContext(context);context=nullptr;}
        if(previous_context)wglMakeCurrent(previous_dc,previous_context);
        if(dc&&window){ReleaseDC(window,dc);dc=nullptr;}
        if(window){DestroyWindow(window);window=nullptr;}
        if(registered){UnregisterClassA(Name,GetModuleHandleA(nullptr));registered=false;}
    }
    ~Context(){Cleanup();}
};
struct Shader{GLuint id=0;~Shader(){if(id)glDeleteShader(id);}};
struct Objects {
    std::array<GLuint,6> programs{};
    std::array<GLuint,4> textures{}; // Native colour, selected colour row, fade, dummy lightmap.
    std::vector<GLuint> sources;
    std::array<GLuint,2> vaos{},buffers{};
    GLuint fbo=0,depth=0;
    ~Objects(){
        glUseProgram(0);glBindVertexArray(0);glBindFramebuffer(GL_FRAMEBUFFER,0);
        for(auto p:programs)if(p)glDeleteProgram(p);
        glDeleteTextures(GLsizei(textures.size()),textures.data());
        if(!sources.empty())glDeleteTextures(GLsizei(sources.size()),sources.data());
        glDeleteVertexArrays(GLsizei(vaos.size()),vaos.data());glDeleteBuffers(GLsizei(buffers.size()),buffers.data());
        if(depth)glDeleteRenderbuffers(1,&depth);
        if(fbo)glDeleteFramebuffers(1,&fbo);
    }
};
void Compile(Shader& shader,GLenum kind,const char* source)
{
    shader.id=glCreateShader(kind);Require(shader.id!=0,"Create world overlay reference shader");
    glShaderSource(shader.id,1,&source,nullptr);glCompileShader(shader.id);
    GLint okay=0;glGetShaderiv(shader.id,GL_COMPILE_STATUS,&okay);
    if(!okay){std::array<char,8192> log{};glGetShaderInfoLog(shader.id,GLsizei(log.size()),nullptr,log.data());
        throw std::runtime_error(std::string("Production world overlay shader compile: ")+log.data());}
}
void Program(GLuint& program,const char* vertex,const char* fragment)
{
    Shader v,f;Compile(v,GL_VERTEX_SHADER,vertex);Compile(f,GL_FRAGMENT_SHADER,fragment);
    program=glCreateProgram();Require(program!=0,"Create world overlay reference program");
    glAttachShader(program,v.id);glAttachShader(program,f.id);glLinkProgram(program);
    GLint okay=0;glGetProgramiv(program,GL_LINK_STATUS,&okay);
    if(!okay){std::array<char,8192> log{};glGetProgramInfoLog(program,GLsizei(log.size()),nullptr,log.data());
        throw std::runtime_error(std::string("Production world overlay program link: ")+log.data());}
}
void Texture(GLenum unit,GLuint id,GLint internal,uint32_t width,uint32_t height,GLenum format,GLenum type,const void* pixels)
{
    glActiveTexture(unit);glBindTexture(GL_TEXTURE_2D,id);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D,0,internal,width,height,0,format,type,pixels);
}
void Attribute(unsigned index,int count,int stride,size_t offset)
{
    glEnableVertexAttribArray(index);glVertexAttribPointer(index,count,GL_FLOAT,GL_FALSE,stride,reinterpret_cast<void*>(offset));
}
void Validate(const WorldOverlayPass& pass,const Image& background,const std::vector<OutlineDepthTriangle>& terrain)
{
    ValidateWorldOverlayPass(pass);
    if(background.width!=pass.native_width||background.height!=pass.native_height
        ||background.rgba.size()!=uint64_t(pass.native_width)*pass.native_height*4||terrain.size()>262144)
        throw std::invalid_argument("Invalid world overlay background or terrain count");
    for(const auto& t:terrain)for(const auto& v:t.vertices)
        if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z)||!std::isfinite(v.w)
            ||std::abs(v.x)>65536||std::abs(v.y)>65536||std::abs(v.z)>16||v.w<1||v.w>2147483648.0f||std::floor(v.w)!=v.w)
            throw std::invalid_argument("Invalid world overlay terrain vertex");
}
}
std::string WorldOverlayGLReferenceDevice(){return device_identity;}
Image RenderWorldOverlayGLReference(const WorldOverlayPass& pass,const Image& background,const std::vector<OutlineDepthTriangle>& terrain)
{
    Validate(pass,background,terrain); // Before staging, context or any GL allocation.
    std::vector<uint8_t> pixels(background.rgba.size());
    const size_t stride=size_t(background.width)*4;
    for(uint32_t y=0;y<background.height;++y)
        std::copy_n(background.rgba.data()+size_t(y)*stride,stride,pixels.data()+size_t(background.height-1-y)*stride);
    std::lock_guard lock(oracle_mutex);Context context;Objects objects;
    constexpr const char* fragments[]={UI_SPRITE_FRAGMENT_SHADER,UI_SPRITE_COLORED_FRAGMENT_SHADER,
        UI_SOLID_FRAGMENT_SHADER,UI_REMAP_FRAGMENT_SHADER,UI_CLUT_FRAGMENT_SHADER};
    for(unsigned i=0;i<5;++i)Program(objects.programs[i],UI_VERTEX_SHADER,fragments[i]);
    Program(objects.programs[5],WORLD_VERTEX_SHADER,"#version 330 core\nout vec4 colour;void main(){colour=vec4(0.0);}");
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);glPixelStorei(GL_PACK_ALIGNMENT,1);
    glGenTextures(GLsizei(objects.textures.size()),objects.textures.data());
    objects.sources.resize(pass.sources.size());
    if(!objects.sources.empty())glGenTextures(GLsizei(objects.sources.size()),objects.sources.data());
    Texture(GL_TEXTURE0,objects.textures[0],GL_RGBA8,background.width,background.height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    glGenRenderbuffers(1,&objects.depth);glBindRenderbuffer(GL_RENDERBUFFER,objects.depth);
    glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,background.width,background.height);
    glGenFramebuffers(1,&objects.fbo);glBindFramebuffer(GL_FRAMEBUFFER,objects.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,objects.textures[0],0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,objects.depth);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"Incomplete world overlay D24 framebuffer");
    glDrawBuffer(GL_COLOR_ATTACHMENT0);glReadBuffer(GL_COLOR_ATTACHMENT0);
    for(size_t i=0;i<pass.sources.size();++i){const auto& source=pass.sources[i];
        Texture(GL_TEXTURE0,objects.sources[i],GL_R8,source.width,source.height,GL_RED,GL_UNSIGNED_BYTE,source.indices.data());}
    if(!pass.fade.empty())Texture(GL_TEXTURE2,objects.textures[2],GL_R8,256,64,GL_RED,GL_UNSIGNED_BYTE,pass.fade.data());
    const uint16_t light=0;Texture(GL_TEXTURE3,objects.textures[3],GL_R16UI,1,1,GL_RED_INTEGER,GL_UNSIGNED_SHORT,&light);
    glGenVertexArrays(GLsizei(objects.vaos.size()),objects.vaos.data());glGenBuffers(GLsizei(objects.buffers.size()),objects.buffers.data());
    glViewport(0,0,background.width,background.height);glDisable(GL_SCISSOR_TEST);glDisable(GL_CULL_FACE);
    glDisable(GL_MULTISAMPLE);glDisable(GL_STENCIL_TEST);glDisable(GL_FRAMEBUFFER_SRGB);glEnable(GL_DITHER);
    glDisable(GL_BLEND);glEnable(GL_DEPTH_TEST);glDepthRange(0,1);glDepthMask(GL_TRUE);glClearDepth(1);
    glClear(GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);glDepthFunc(GL_ALWAYS);glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);
    glUseProgram(objects.programs[5]);glUniform1i(glGetUniformLocation(objects.programs[5],"u_lighting_mode"),0);
    glUniform1i(glGetUniformLocation(objects.programs[5],"u_lightmap"),3);
    glBindVertexArray(objects.vaos[0]);glBindBuffer(GL_ARRAY_BUFFER,objects.buffers[0]);
    glBufferData(GL_ARRAY_BUFFER,GLsizeiptr(terrain.size()*sizeof(OutlineDepthTriangle)),terrain.data(),GL_STREAM_DRAW);
    Attribute(0,3,sizeof(OutlineDepthVertex),0);Attribute(4,1,sizeof(OutlineDepthVertex),offsetof(OutlineDepthVertex,w));
    if(!terrain.empty())glDrawArrays(GL_TRIANGLES,0,GLsizei(terrain.size()*3));
    CheckGL("World overlay oracle painter terrain depth");
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glDepthMask(GL_FALSE);glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(objects.vaos[1]);glBindBuffer(GL_ARRAY_BUFFER,objects.buffers[1]);
    Attribute(0,2,sizeof(WorldOverlayVertex),0);Attribute(1,2,sizeof(WorldOverlayVertex),8);
    Attribute(2,4,sizeof(WorldOverlayVertex),16);Attribute(3,1,sizeof(WorldOverlayVertex),32);
    for(const auto& q:pass.draws){
        if(q.depth_test){glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LEQUAL);}else glDisable(GL_DEPTH_TEST);
        if(q.scissor_enabled){glEnable(GL_SCISSOR_TEST);glScissor(q.scissor_x,int(background.height)-q.scissor_y-q.scissor_h,q.scissor_w,q.scissor_h);}
        else glDisable(GL_SCISSOR_TEST);
        const GLuint program=objects.programs[unsigned(q.mode)];glUseProgram(program);
        glUniform2f(glGetUniformLocation(program,"u_screen_size"),float(background.width),float(background.height));
        glUniform1i(glGetUniformLocation(program,"u_sprite_atlas"),0);
        if(q.source!=NoWorldOverlayResource){glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,objects.sources[q.source]);}
        if(q.colour_row!=NoWorldOverlayResource){
            // Only the selected owned row is bound. Production normalized X
            // lookup and alpha semantics still run in the original shaders.
            Texture(GL_TEXTURE1,objects.textures[1],GL_RGBA8,256,1,GL_RGBA,GL_UNSIGNED_BYTE,pass.colour_rows[q.colour_row].data());
            glUniform1i(glGetUniformLocation(program,"u_palette"),1);glUniform1i(glGetUniformLocation(program,"u_clut"),1);
            glUniform1f(glGetUniformLocation(program,"u_clut_v"),0.5f);
        }
        glUniform1i(glGetUniformLocation(program,"u_fade_table"),2);glUniform1f(glGetUniformLocation(program,"u_remap_row"),float(q.fade_row));
        const auto vertex=[&](float x,float y,float u,float v){return WorldOverlayVertex{x,y,u,v,q.colour,q.z_ndc};};
        const WorldOverlayVertex vertices[]={vertex(q.x0,q.y0,q.u0,q.v0),vertex(q.x1,q.y0,q.u1,q.v0),vertex(q.x1,q.y1,q.u1,q.v1),
            vertex(q.x0,q.y0,q.u0,q.v0),vertex(q.x1,q.y1,q.u1,q.v1),vertex(q.x0,q.y1,q.u0,q.v1)};
        glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_STREAM_DRAW);glDrawArrays(GL_TRIANGLES,0,6);
        CheckGL("Production world overlay quad");
    }
    glDisable(GL_SCISSOR_TEST);glReadPixels(0,0,background.width,background.height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    CheckGL("World overlay oracle readback");
    Image result{background.width,background.height,std::vector<uint8_t>(pixels.size())};
    for(uint32_t y=0;y<background.height;++y)
        std::copy_n(pixels.data()+size_t(background.height-1-y)*stride,stride,result.rgba.data()+size_t(y)*stride);
    return result;
}
}
