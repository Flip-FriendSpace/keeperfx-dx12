// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// The original production shaders remain in GLShaders.h; this file supplies
// only owned inputs, an isolated GL context, and explicit diagnostic draw state.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <glad/glad.h>
#include "OutlineGLOracle.h"
#include "EnhancedWorldGLOracle.h"
#include "kfx/renderer/opengl/GLShaders.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace kfx::render::dx12 {
namespace {
std::mutex oracle_mutex; // GLAD's function pointer table is process-global.
thread_local std::string device_identity;
void Require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}
void CheckGL(const char* operation)
{
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR)
        throw std::runtime_error(std::string(operation) + ": GL error " + std::to_string(error));
}
void* LoadGL(const char* name)
{
    void* address = reinterpret_cast<void*>(wglGetProcAddress(name));
    if (!address || address == reinterpret_cast<void*>(1) || address == reinterpret_cast<void*>(2)
        || address == reinterpret_cast<void*>(3) || address == reinterpret_cast<void*>(-1))
        address = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("opengl32.dll"), name));
    return address;
}
struct Context {
    HWND window = nullptr;
    HDC dc = nullptr, previous_dc = nullptr;
    HGLRC context = nullptr, previous_context = nullptr;
    bool registered = false;
    static constexpr const char* ClassName = "KeeperFXOwnedOutlineOracle";
    Context()
    {
        previous_context = wglGetCurrentContext(); previous_dc = wglGetCurrentDC();
        try {
            WNDCLASSA wc{};
            wc.style = CS_OWNDC; wc.lpfnWndProc = DefWindowProcA;
            wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = ClassName;
            registered = RegisterClassA(&wc) != 0;
            Require(registered || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "Register outline oracle window");
            window = CreateWindowA(ClassName, "Owned outline reference", WS_OVERLAPPEDWINDOW,
                0, 0, 32, 32, nullptr, nullptr, wc.hInstance, nullptr);
            Require(window != nullptr, "Create hidden outline oracle window");
            dc = GetDC(window); Require(dc != nullptr, "Get outline oracle DC");
            PIXELFORMATDESCRIPTOR format{};
            format.nSize = sizeof(format); format.nVersion = 1;
            format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
            format.iPixelType = PFD_TYPE_RGBA; format.cColorBits = 32;
            format.cDepthBits = 24; format.cStencilBits = 8;
            const int pixel_format = ChoosePixelFormat(dc, &format);
            Require(pixel_format && SetPixelFormat(dc, pixel_format, &format), "Set outline oracle pixel format");
            context = wglCreateContext(dc);
            Require(context && wglMakeCurrent(dc, context), "Create hidden outline oracle context");
            Require(gladLoadGLLoader(LoadGL) && GLAD_GL_VERSION_3_3, "Outline oracle requires OpenGL 3.3");
            CheckGL("Initialize outline GL loader");
            const auto text = [](GLenum name) {
                const auto* value = glGetString(name);
                Require(value != nullptr, "Missing outline oracle device string");
                return std::string(reinterpret_cast<const char*>(value));
            };
            device_identity = text(GL_VENDOR) + "; " + text(GL_RENDERER) + "; " + text(GL_VERSION);
        } catch (...) { Cleanup(); throw; }
    }
    void Cleanup() noexcept
    {
        if (context) {
            wglMakeCurrent(nullptr, nullptr); wglDeleteContext(context); context = nullptr;
        }
        if (previous_context) wglMakeCurrent(previous_dc, previous_context);
        if (dc && window) { ReleaseDC(window, dc); dc = nullptr; }
        if (window) { DestroyWindow(window); window = nullptr; }
        if (registered) { UnregisterClassA(ClassName, GetModuleHandleA(nullptr)); registered = false; }
    }
    ~Context() { Cleanup(); }
};
struct Shader {
    GLuint id = 0;
    ~Shader() { if (id) glDeleteShader(id); }
};
struct Objects {
    std::array<GLuint, 12> programs{};
    std::array<GLuint, 5> textures{};
    std::array<GLuint, 5> buffers{};
    std::array<GLuint, 4> vaos{};
    GLuint framebuffer = 0, depth = 0;
    ~Objects()
    {
        glUseProgram(0); glBindVertexArray(0); glBindFramebuffer(GL_FRAMEBUFFER, 0);
        for (auto program : programs) if (program) glDeleteProgram(program);
        glDeleteTextures(GLsizei(textures.size()), textures.data());
        glDeleteBuffers(GLsizei(buffers.size()), buffers.data());
        glDeleteVertexArrays(GLsizei(vaos.size()), vaos.data());
        if (depth) glDeleteRenderbuffers(1, &depth);
        if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
    }
};
void Compile(Shader& shader, GLenum type, const char* source)
{
    shader.id = glCreateShader(type); Require(shader.id != 0, "Create production outline shader");
    glShaderSource(shader.id, 1, &source, nullptr); glCompileShader(shader.id);
    GLint ok = 0; glGetShaderiv(shader.id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        std::array<char, 8192> log{}; glGetShaderInfoLog(shader.id, GLsizei(log.size()), nullptr, log.data());
        throw std::runtime_error(std::string("Production outline shader compilation: ") + log.data());
    }
}
void Program(GLuint& program, const char* vertex_source, const char* fragment_source)
{
    Shader vertex, fragment;
    Compile(vertex, GL_VERTEX_SHADER, vertex_source); Compile(fragment, GL_FRAGMENT_SHADER, fragment_source);
    program = glCreateProgram(); Require(program != 0, "Create production outline program");
    glAttachShader(program, vertex.id); glAttachShader(program, fragment.id); glLinkProgram(program);
    GLint ok = 0; glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        std::array<char, 8192> log{}; glGetProgramInfoLog(program, GLsizei(log.size()), nullptr, log.data());
        throw std::runtime_error(std::string("Production outline shader link: ") + log.data());
    }
}
void Texture(GLenum target, GLuint texture)
{
    glBindTexture(target, texture);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
void Attribute(GLuint location, GLint components, GLsizei stride, size_t offset, GLuint divisor = 0)
{
    glEnableVertexAttribArray(location);
    glVertexAttribPointer(location, components, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offset));
    glVertexAttribDivisor(location, divisor);
}
void Validate(const Image& background, const Image& atlas,
    const std::vector<OutlineDepthTriangle>& terrain, const std::vector<OutlinePassDraw>& draws,
    const std::vector<SpriteColourRow>& cluts, const std::array<float, 9>& palette_transform, bool enhanced)
{
    const auto valid = [](const Image& image) {
        return image.width && image.height && image.width <= 16384 && image.height <= 16384
            && uint64_t(image.width) * image.height * 4 <= 64 * 1024 * 1024
            && image.rgba.size() == size_t(image.width) * image.height * 4;
    };
    Require(valid(background) && valid(atlas) && terrain.size() <= 262144 && draws.size() <= 196608,
        "Invalid owned outline oracle input");
    Require(cluts.size() <= 16384 && atlas.rgba.size() + uint64_t(cluts.size()) * sizeof(SpriteColourRow) <= 64 * 1024 * 1024,
        "Invalid enhanced sprite lookup budget");
    for (float value : palette_transform)
        Require(std::isfinite(value) && std::abs(value) <= 1024, "Invalid enhanced sprite palette transform");
    for (const auto& triangle : terrain) for (const auto& v : triangle.vertices)
        Require(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w)
            && std::abs(v.x) <= 65536 && std::abs(v.y) <= 65536 && std::abs(v.z) <= 16
            && v.w >= 1 && v.w <= 2147483648.0f, "Invalid outline oracle depth input");
    for (const auto& draw : draws) {
        Require(uint32_t(draw.kind) <= (enhanced ? 5u : 2u) && draw.width > 0 && draw.height > 0 && draw.width <= 16384 && draw.height <= 16384
            && draw.x >= -65536 && draw.x <= 65536 && draw.y >= -65536 && draw.y <= 65536
            && draw.phase_x >= -16777216 && draw.phase_x <= 16777216 && draw.phase_y >= -16777216 && draw.phase_y <= 16777216
            && draw.step_x > 0 && draw.step_x <= 16777216 && draw.step_y > 0 && draw.step_y <= 16777216
            && draw.source_width && draw.source_height && draw.source_width <= 256 && draw.source_height <= 256
            && draw.content_height && draw.content_height <= draw.source_height
            && uint64_t(draw.mask_x) + draw.source_width <= atlas.width && uint64_t(draw.mask_y) + draw.source_height <= atlas.height
            && std::isfinite(draw.z_ndc) && std::abs(draw.z_ndc) <= 16, "Invalid outline oracle sprite mapping");
        const bool normal = draw.kind == OutlinePassKind::NormalSprite;
        Require(!(draw.texture_fallback && draw.premultiplied) && (enhanced || !draw.texture_fallback)
            && std::isfinite(draw.source_alpha) && draw.source_alpha >= 0 && draw.source_alpha <= 1
            && (normal ? draw.lookup_row < cluts.size() : draw.lookup_row == NoSpriteColourRow)
            && (normal || draw.source_alpha == 1), "Inconsistent oracle sprite path or colour binding");
        for (float channel : draw.colour) Require(std::isfinite(channel) && channel >= 0 && channel <= 1, "Invalid outline oracle colour");
        for (uint32_t y = 0; y < draw.source_height; ++y) for (uint32_t x = 0; x < draw.source_width; ++x) {
            const auto value = atlas.rgba[(size_t(draw.mask_y + y) * atlas.width + draw.mask_x + x) * 4 + 1];
            Require(value == 0 || value == 255, "Nonbinary owned outline coverage");
        }
    }
}
struct Instance {
    float rect[4], source[2], mapping[4], layer_z_flip[3], colour[4];
};
static_assert(sizeof(Instance) == 68);
struct FallbackVertex { float position[2], local[2], mapping[4], source[2], flip; };
static_assert(sizeof(FallbackVertex) == 44);
struct ArtInstance { float rect[4], source[2], misc[4]; uint32_t flags; float mapping[4]; };
static_assert(sizeof(ArtInstance) == 60);
}

std::string OutlineGLReferenceDevice() { return device_identity; }

static Image RenderSpritesGL(const Image& background, const Image& atlas,
    const std::vector<OutlineDepthTriangle>& terrain, const std::vector<OutlinePassDraw>& draws,
    const std::vector<SpriteColourRow>& cluts, const std::array<float, 9>& palette_transform, bool enhanced,
    const EnhancedWorldPass* world=nullptr,const SceneGLCompositionTrace* composition=nullptr)
{
    Validate(background, atlas, terrain, draws, cluts, palette_transform, enhanced);
    if(composition){
        Require(world!=nullptr,"Composed GL reference requires an owned enhanced world");
        ValidateEnhancedCompositionGLInputs(*world,atlas.rgba.size(),cluts.size(),*composition);
    }
    std::lock_guard lock(oracle_mutex);
    Context context;
    Objects objects;
    constexpr const char* DepthFragment = "#version 330 core\nout vec4 colour;void main(){colour=vec4(0.0);}";
    Program(objects.programs[0], WORLD_VERTEX_SHADER, DepthFragment);
    Program(objects.programs[1], KSPR_INST_OUTLINE_VERTEX_SHADER, KSPR_INST_OUTLINE_FRAGMENT_SHADER);
    Program(objects.programs[2], KSPR_INST_OUTLINE_VERTEX_SHADER, KSPR_INST_EDGE_FRAGMENT_SHADER);
    Program(objects.programs[3], KSPR_VERTEX_SHADER, KSPR_OUTLINE_FRAGMENT_SHADER);
    Program(objects.programs[4], KSPR_VERTEX_SHADER, KSPR_EDGE_FRAGMENT_SHADER);
    if (enhanced) {
        Program(objects.programs[5], KSPR_INST_VERTEX_SHADER, KSPR_INST_FRAGMENT_SHADER);
        Program(objects.programs[6], KSPR_VERTEX_SHADER, KSPR_ARRAY_FRAGMENT_SHADER);
        Program(objects.programs[7], KSPR_VERTEX_SHADER, KSPR_FRAGMENT_SHADER);
        Program(objects.programs[8], KSPR_VERTEX_SHADER, KSPR_ARRAY_GLOW_FRAGMENT_SHADER);
        Program(objects.programs[9], KSPR_VERTEX_SHADER, KSPR_GLOW_FRAGMENT_SHADER);
        Program(objects.programs[10], KSPR_VERTEX_SHADER, KSPR_ARRAY_OUTLINE_FRAGMENT_SHADER);
        Program(objects.programs[11], KSPR_VERTEX_SHADER, KSPR_ARRAY_EDGE_FRAGMENT_SHADER);
    }
    glGenTextures(GLsizei(objects.textures.size()), objects.textures.data());
    glGenVertexArrays(GLsizei(objects.vaos.size()), objects.vaos.data());
    glGenBuffers(GLsizei(objects.buffers.size()), objects.buffers.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glActiveTexture(GL_TEXTURE0);
    Texture(GL_TEXTURE_2D, objects.textures[0]);
    // GL framebuffer row zero is the bottom. Source sprites remain top-down;
    // their integer texelFetch coordinates are independent of framebuffer rows.
    std::vector<uint8_t> framebuffer_pixels(background.rgba.size());
    const size_t row_bytes = size_t(background.width) * 4;
    for (uint32_t y = 0; y < background.height; ++y)
        std::copy_n(background.rgba.data() + size_t(y) * row_bytes, row_bytes,
            framebuffer_pixels.data() + size_t(background.height - 1 - y) * row_bytes);
    const auto target_width=world?world->native_width:background.width;
    const auto target_height=world?world->native_height:background.height;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, target_width, target_height, 0,
        GL_RGBA, GL_UNSIGNED_BYTE,world?nullptr:framebuffer_pixels.data());
    glGenRenderbuffers(1, &objects.depth); glBindRenderbuffer(GL_RENDERBUFFER, objects.depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, target_width, target_height);
    glGenFramebuffers(1, &objects.framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, objects.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, objects.textures[0], 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, objects.depth);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Incomplete outline oracle D24 framebuffer");
    glDrawBuffer(GL_COLOR_ATTACHMENT0); glReadBuffer(GL_COLOR_ATTACHMENT0);
    Texture(GL_TEXTURE_2D, objects.textures[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, 256, 256, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
    Texture(GL_TEXTURE_2D_ARRAY, objects.textures[2]);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RG8, 256, 256, 1, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
    glActiveTexture(GL_TEXTURE1); Texture(GL_TEXTURE_2D, objects.textures[3]);
    const uint16_t light = 0;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, 1, 1, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, &light);
    const GLint origin_x=world?world->viewport_x:0;
    const GLint origin_y=world?GLint(world->native_height)-world->viewport_y-GLint(world->viewport_height):0;
    glViewport(origin_x,origin_y,background.width,background.height);
    glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    glDisable(GL_MULTISAMPLE); glDisable(GL_FRAMEBUFFER_SRGB);
    glEnable(GL_DITHER); // Production GL leaves the default dither state enabled.
    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
    glDepthRange(0, 1); glClearDepth(1); glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if(world){
        glClearColor(world->clear_rgba[0],world->clear_rgba[1],world->clear_rgba[2],world->clear_rgba[3]);
        glClear(GL_COLOR_BUFFER_BIT);
        DrawEnhancedWorldGLStages(*world,atlas);
    }else{
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE); glDepthFunc(GL_ALWAYS);
    glUseProgram(objects.programs[0]);
    glUniform1i(glGetUniformLocation(objects.programs[0], "u_lighting_mode"), 0);
    glUniform1i(glGetUniformLocation(objects.programs[0], "u_lightmap"), 1);
    glBindVertexArray(objects.vaos[0]); glBindBuffer(GL_ARRAY_BUFFER, objects.buffers[0]);
    static_assert(sizeof(OutlineDepthVertex) == 16 && sizeof(OutlineDepthTriangle) == 48);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(terrain.size() * sizeof(OutlineDepthTriangle)), terrain.data(), GL_STREAM_DRAW);
    Attribute(0, 3, sizeof(OutlineDepthVertex), 0);
    Attribute(4, 1, sizeof(OutlineDepthVertex), offsetof(OutlineDepthVertex, w));
    if (!terrain.empty()) glDrawArrays(GL_TRIANGLES, 0, GLsizei(terrain.size() * 3));
    CheckGL("Outline oracle terrain depth");
    }

    // Production instanced and fallback paths use different diagonals.
    const float unit_strip[] = {0,0, 1,0, 0,1, 1,1};
    const float fallback_quad[] = {0,0, 1,0, 1,1, 0,0, 1,1, 0,1};
    glBindVertexArray(objects.vaos[1]); glBindBuffer(GL_ARRAY_BUFFER, objects.buffers[1]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(unit_strip), unit_strip, GL_STATIC_DRAW);
    Attribute(0, 2, 2 * sizeof(float), 0);
    glBindBuffer(GL_ARRAY_BUFFER, objects.buffers[2]);
    Attribute(1, 4, sizeof(Instance), offsetof(Instance, rect), 1);
    Attribute(2, 2, sizeof(Instance), offsetof(Instance, source), 1);
    Attribute(3, 3, sizeof(Instance), offsetof(Instance, layer_z_flip), 1);
    Attribute(4, 4, sizeof(Instance), offsetof(Instance, colour), 1);
    Attribute(5, 4, sizeof(Instance), offsetof(Instance, mapping), 1);
    glBindVertexArray(objects.vaos[2]); glBindBuffer(GL_ARRAY_BUFFER, objects.buffers[3]);
    Attribute(0, 2, sizeof(FallbackVertex), offsetof(FallbackVertex, position));
    Attribute(1, 2, sizeof(FallbackVertex), offsetof(FallbackVertex, local));
    Attribute(2, 4, sizeof(FallbackVertex), offsetof(FallbackVertex, mapping));
    Attribute(3, 2, sizeof(FallbackVertex), offsetof(FallbackVertex, source));
    Attribute(4, 1, sizeof(FallbackVertex), offsetof(FallbackVertex, flip));
    if (enhanced) {
        glBindVertexArray(objects.vaos[3]); glBindBuffer(GL_ARRAY_BUFFER, objects.buffers[1]);
        Attribute(0, 2, 2 * sizeof(float), 0);
        glBindBuffer(GL_ARRAY_BUFFER, objects.buffers[4]);
        Attribute(1, 4, sizeof(ArtInstance), offsetof(ArtInstance, rect), 1);
        Attribute(2, 2, sizeof(ArtInstance), offsetof(ArtInstance, source), 1);
        Attribute(3, 4, sizeof(ArtInstance), offsetof(ArtInstance, misc), 1);
        glEnableVertexAttribArray(4);
        glVertexAttribIPointer(4, 1, GL_UNSIGNED_INT, sizeof(ArtInstance), reinterpret_cast<void*>(offsetof(ArtInstance, flags)));
        glVertexAttribDivisor(4, 1);
        Attribute(5, 4, sizeof(ArtInstance), offsetof(ArtInstance, mapping), 1);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); glDepthMask(GL_FALSE);
    glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD);
    if (enhanced) {
        // Keep the normal sampler valid even when the first/all instances are
        // glow and its dynamically selected branch never reads the CLUT.
        const SpriteColourRow zero{};
        glActiveTexture(GL_TEXTURE1); Texture(GL_TEXTURE_2D, objects.textures[4]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero.data());
    }
    glActiveTexture(GL_TEXTURE0);
    std::array<uint8_t, 256 * 256 * 2> source{};
    for (const auto& draw : draws) {
        source.fill(0);
        for (uint32_t y = 0; y < draw.source_height; ++y) for (uint32_t x = 0; x < draw.source_width; ++x) {
            const auto input = (size_t(draw.mask_y + y) * atlas.width + draw.mask_x + x) * 4;
            const auto output = (size_t(y) * 256 + x) * 2;
            source[output] = atlas.rgba[input]; source[output + 1] = atlas.rgba[input + 1];
        }
        const bool edge = draw.kind == OutlinePassKind::Edge;
        const bool normal = draw.kind == OutlinePassKind::NormalSprite;
        const bool glow = draw.kind == OutlinePassKind::GlowAdd || draw.kind == OutlinePassKind::GlowDarken;
        const bool darken = draw.kind == OutlinePassKind::GlowDarken;
        const bool art = normal || glow;
        const bool array = draw.premultiplied || (enhanced && !draw.texture_fallback);
        size_t program_index = (draw.premultiplied ? 1 : array ? 10 : 3) + unsigned(edge);
        if (art) program_index = draw.premultiplied ? 5 : (glow ? 8 : 6) + unsigned(draw.texture_fallback);
        const GLuint program = objects.programs[program_index];
        glUseProgram(program);
        glUniform2f(glGetUniformLocation(program, "u_viewport"), float(background.width), float(background.height));
        glUniform1i(glGetUniformLocation(program, "u_sprite"), 0);
        glDepthFunc(art || draw.kind == OutlinePassKind::VisibleCoverage ? GL_LEQUAL : GL_GREATER);
        // Match RGB AND alpha: fallback's alpha contribution is source A*A.
        glBlendFunc(draw.premultiplied ? GL_ONE : GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBlendEquation(darken ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
        if (glow) glBlendFunc(GL_ONE, draw.premultiplied ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE);
        if (art) {
            glUniform1f(glGetUniformLocation(program, "u_alpha"), draw.source_alpha);
            glUniform1f(glGetUniformLocation(program, "u_layer"), 0);
            glUniform1f(glGetUniformLocation(program, "u_clut_v"), 0.5f);
            glUniform1i(glGetUniformLocation(program, "u_clut"), 1);
            glUniform1i(glGetUniformLocation(program, "u_palette"), 1);
            glUniform1i(glGetUniformLocation(program, "u_darken"), darken);
            glUniformMatrix3fv(glGetUniformLocation(program, "u_palette_xform"), 1, GL_TRUE, palette_transform.data());
            if (normal) {
                glActiveTexture(GL_TEXTURE1); Texture(GL_TEXTURE_2D, objects.textures[4]);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, cluts[draw.lookup_row].data());
                glActiveTexture(GL_TEXTURE0);
            }
        }
        if (array) {
            glBindTexture(GL_TEXTURE_2D_ARRAY, objects.textures[2]);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 256, 256, 1, GL_RG, GL_UNSIGNED_BYTE, source.data());
        } else {
            glBindTexture(GL_TEXTURE_2D, objects.textures[1]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RG, GL_UNSIGNED_BYTE, source.data());
        }
        if (draw.premultiplied && art) {
            ArtInstance instance{{float(draw.x),float(draw.y),float(draw.width),float(draw.height)},
                {float(draw.source_width),float(draw.content_height)}, {0,0.5f,draw.source_alpha,draw.z_ndc},
                uint32_t(draw.flip) | (glow ? 2u : 0u) | (darken ? 4u : 0u),
                {float(draw.phase_x),float(draw.phase_y),float(draw.step_x),float(draw.step_y)}};
            glBindVertexArray(objects.vaos[3]); glBindBuffer(GL_ARRAY_BUFFER, objects.buffers[4]);
            glBufferData(GL_ARRAY_BUFFER, sizeof(instance), &instance, GL_STREAM_DRAW);
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 1);
        } else if (draw.premultiplied) {
            Instance instance{{float(draw.x), float(draw.y), float(draw.width), float(draw.height)},
                {float(draw.source_width), float(draw.content_height)},
                {float(draw.phase_x), float(draw.phase_y), float(draw.step_x), float(draw.step_y)},
                {0, draw.z_ndc, float(draw.flip)}, {}};
            std::copy(draw.colour.begin(), draw.colour.end(), instance.colour);
            glBindVertexArray(objects.vaos[1]); glBindBuffer(GL_ARRAY_BUFFER, objects.buffers[2]);
            glBufferData(GL_ARRAY_BUFFER, sizeof(instance), &instance, GL_STREAM_DRAW);
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 1);
        } else {
            glUniform1f(glGetUniformLocation(program, "u_z_ndc"), draw.z_ndc);
            glUniform4fv(glGetUniformLocation(program, "u_outline_color"), 1, draw.colour.data());
            std::array<FallbackVertex, 6> vertices{};
            for (size_t i = 0; i < vertices.size(); ++i) {
                const float x = fallback_quad[i * 2] * draw.width, y = fallback_quad[i * 2 + 1] * draw.height;
                vertices[i] = {{float(draw.x) + x, float(draw.y) + y}, {x,y},
                    {float(draw.phase_x), float(draw.phase_y), float(draw.step_x), float(draw.step_y)},
                    {float(draw.source_width), float(draw.content_height)}, float(draw.flip)};
            }
            glBindVertexArray(objects.vaos[2]); glBindBuffer(GL_ARRAY_BUFFER, objects.buffers[3]);
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices.data(), GL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
        CheckGL("Outline oracle sprite phase");
    }
    if(composition)return FinishEnhancedCompositionGLReference(*composition,objects.textures[0]);
    glReadPixels(origin_x,origin_y,background.width,background.height,GL_RGBA,GL_UNSIGNED_BYTE,framebuffer_pixels.data());
    CheckGL("Outline oracle RGBA readback");
    Image result{background.width, background.height, std::vector<uint8_t>(background.rgba.size())};
    for (uint32_t y = 0; y < background.height; ++y)
        std::copy_n(framebuffer_pixels.data() + size_t(background.height - 1 - y) * row_bytes, row_bytes,
            result.rgba.data() + size_t(y) * row_bytes);
    return result;
}
Image RenderOutlineGLReference(const Image& background, const Image& atlas,
    const std::vector<OutlineDepthTriangle>& terrain, const std::vector<OutlinePassDraw>& draws)
{
    return RenderSpritesGL(background, atlas, terrain, draws, {}, {1,0,0,0,1,0,0,0,1}, false);
}
Image RenderEnhancedSpritesGLReference(const Image& background, const Image& atlas,
    const std::vector<SpriteColourRow>& cluts, const std::array<float, 9>& palette_transform,
    const std::vector<OutlineDepthTriangle>& terrain, const std::vector<OutlinePassDraw>& draws)
{
    return RenderSpritesGL(background, atlas, terrain, draws, cluts, palette_transform, true);
}
Image RenderEnhancedWorldGLReference(const EnhancedWorldPass& world,const Image& atlas,
    const std::vector<SpriteColourRow>& cluts,const std::array<float,9>& palette_transform,
    const std::vector<OutlinePassDraw>& draws)
{
    ValidateEnhancedWorldPass(world,atlas.width,atlas.height,atlas.rgba.size(),cluts.size());
    for(size_t i=1;i<atlas.rgba.size();i+=4)
        Require(atlas.rgba[i]==0||atlas.rgba[i]==255,"Nonbinary owned world coverage");
    // Only dimensions and storage are shared with the old diagnostic runner.
    // No composed background is uploaded: the native GL target is cleared.
    const Image dimensions{world.viewport_width,world.viewport_height,
        std::vector<uint8_t>(size_t(world.viewport_width)*world.viewport_height*4)};
    return RenderSpritesGL(dimensions,atlas,{},draws,cluts,palette_transform,true,&world);
}
Image RenderEnhancedCompositionGLReference(const EnhancedWorldPass& world,const Image& atlas,
    const std::vector<SpriteColourRow>& cluts,const std::array<float,9>& palette_transform,
    const std::vector<OutlinePassDraw>& draws,const SceneGLCompositionTrace& composition)
{
    ValidateEnhancedWorldPass(world,atlas.width,atlas.height,atlas.rgba.size(),cluts.size());
    ValidateEnhancedCompositionGLInputs(world,atlas.rgba.size(),cluts.size(),composition);
    for(size_t i=1;i<atlas.rgba.size();i+=4)
        Require(atlas.rgba[i]==0||atlas.rgba[i]==255,"Nonbinary owned composed world coverage");
    const Image dimensions{world.viewport_width,world.viewport_height,
        std::vector<uint8_t>(size_t(world.viewport_width)*world.viewport_height*4)};
    return RenderSpritesGL(dimensions,atlas,{},draws,cluts,palette_transform,true,&world,&composition);
}
}
