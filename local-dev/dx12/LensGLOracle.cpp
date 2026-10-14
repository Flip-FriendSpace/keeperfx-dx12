// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Only context, resources and draw state are supplied here. Lens pixel math
// comes from the unmodified production shader strings in GLShaders.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <glad/glad.h>
#include "LensGLOracle.h"
#include "kfx/renderer/opengl/GLShaders.h"
#include <algorithm>
#include <mutex>
#include <stdexcept>

namespace kfx::render::dx12 {
namespace {
std::mutex oracle_mutex;
thread_local std::string device_identity;
void Require(bool value, const char* reason)
{
    if (!value) throw std::runtime_error(reason);
}
void CheckGL(const char* operation)
{
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR)
        throw std::runtime_error(std::string(operation) + ": GL error " + std::to_string(error));
}
void* LoadGL(const char* name)
{
    void* result = reinterpret_cast<void*>(wglGetProcAddress(name));
    if (!result || result == reinterpret_cast<void*>(1) || result == reinterpret_cast<void*>(2)
        || result == reinterpret_cast<void*>(3) || result == reinterpret_cast<void*>(-1))
        result = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("opengl32.dll"), name));
    return result;
}
struct Context {
    HWND window = nullptr;
    HDC dc = nullptr, previous_dc = nullptr;
    HGLRC context = nullptr, previous_context = nullptr;
    bool registered = false;
    static constexpr const char* ClassName = "KeeperFXOwnedLensOracle";
    Context()
    {
        previous_context = wglGetCurrentContext(); previous_dc = wglGetCurrentDC();
        try {
            WNDCLASSA wc{};
            wc.style = CS_OWNDC; wc.lpfnWndProc = DefWindowProcA;
            wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = ClassName;
            registered = RegisterClassA(&wc) != 0;
            Require(registered || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "Register lens oracle window");
            window = CreateWindowA(ClassName, "Owned lens reference", WS_OVERLAPPEDWINDOW,
                0, 0, 32, 32, nullptr, nullptr, wc.hInstance, nullptr);
            Require(window != nullptr, "Create hidden lens oracle window");
            dc = GetDC(window); Require(dc != nullptr, "Get lens oracle DC");
            PIXELFORMATDESCRIPTOR format{};
            format.nSize = sizeof(format); format.nVersion = 1;
            format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
            format.iPixelType = PFD_TYPE_RGBA; format.cColorBits = 32;
            const int pixel_format = ChoosePixelFormat(dc, &format);
            Require(pixel_format && SetPixelFormat(dc, pixel_format, &format), "Set lens oracle pixel format");
            context = wglCreateContext(dc);
            Require(context && wglMakeCurrent(dc, context), "Create hidden lens oracle context");
            Require(gladLoadGLLoader(LoadGL) && GLAD_GL_VERSION_3_3, "Lens oracle requires OpenGL 3.3");
            CheckGL("Initialize lens oracle GL loader");
            const auto text = [](GLenum name) {
                const auto* value = glGetString(name);
                Require(value != nullptr, "Missing lens oracle device string");
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
    GLuint program = 0, framebuffer = 0, vao = 0, buffer = 0;
    std::array<GLuint, 6> textures{}; // source, target, auxiliary, palette, RGB6, fade
    ~Objects()
    {
        glUseProgram(0); glBindVertexArray(0); glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (program) glDeleteProgram(program);
        if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
        if (vao) glDeleteVertexArrays(1, &vao);
        if (buffer) glDeleteBuffers(1, &buffer);
        glDeleteTextures(GLsizei(textures.size()), textures.data());
    }
};
void Compile(Shader& shader, GLenum kind, const char* source)
{
    shader.id = glCreateShader(kind); Require(shader.id != 0, "Create production lens shader");
    glShaderSource(shader.id, 1, &source, nullptr); glCompileShader(shader.id);
    GLint success = 0; glGetShaderiv(shader.id, GL_COMPILE_STATUS, &success);
    if (!success) {
        std::array<char, 8192> log{};
        glGetShaderInfoLog(shader.id, GLsizei(log.size()), nullptr, log.data());
        throw std::runtime_error(std::string("Production lens shader compile: ") + log.data());
    }
}
void Program(GLuint& program, const char* fragment)
{
    Shader vertex, pixel;
    Compile(vertex, GL_VERTEX_SHADER, LENS_COMPOSITE_VERTEX_SHADER);
    Compile(pixel, GL_FRAGMENT_SHADER, fragment);
    program = glCreateProgram(); Require(program != 0, "Create production lens program");
    glAttachShader(program, vertex.id); glAttachShader(program, pixel.id); glLinkProgram(program);
    GLint success = 0; glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        std::array<char, 8192> log{};
        glGetProgramInfoLog(program, GLsizei(log.size()), nullptr, log.data());
        throw std::runtime_error(std::string("Production lens program link: ") + log.data());
    }
}
std::vector<uint8_t> BottomUp(const Image& image)
{
    std::vector<uint8_t> pixels(image.rgba.size());
    const size_t stride = size_t(image.width) * 4;
    for (uint32_t y = 0; y < image.height; ++y)
        std::copy_n(image.rgba.data() + size_t(y) * stride, stride,
            pixels.data() + size_t(image.height - 1 - y) * stride);
    return pixels;
}
void Texture(GLenum unit, GLuint texture, GLint internal_format, uint32_t width, uint32_t height,
    GLenum format, GLenum type, const void* pixels)
{
    glActiveTexture(unit); glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, GLsizei(width), GLsizei(height), 0, format, type, pixels);
}
}
std::string LensGLReferenceDevice() { return device_identity; }

Image RenderLensGLReference(const LensPass& pass, const Image& native_source, const Image& native_target_background)
{
    ValidateLensPass(pass);
    const auto matches = [](const Image& image, uint32_t width, uint32_t height) {
        return image.width == width && image.height == height
            && image.rgba.size() == uint64_t(width) * height * 4;
    };
    Require(matches(native_source, pass.source_width, pass.source_height)
        && matches(native_target_background, pass.target_width, pass.target_height), "Lens oracle native image extent or byte count mismatch");
    // Validation above precedes CPU staging allocation, context creation and
    // every GL call. Lookup/remap rows stay top-down: their fetch coordinates
    // have their own logical origin, independent of the framebuffer origin.
    auto source = BottomUp(native_source);
    auto target = BottomUp(native_target_background);
    std::lock_guard lock(oracle_mutex);
    Context context;
    Objects objects;
    const char* fragment = LENS_COPY_FRAGMENT_SHADER;
    if (pass.kind == LensPassKind::Mist) fragment = LENS_MIST_FRAGMENT_SHADER;
    else if (pass.kind == LensPassKind::Remap) fragment = LENS_REMAP_FRAGMENT_SHADER;
    else if (pass.kind == LensPassKind::Overlay) fragment = LENS_OVERLAY_FRAGMENT_SHADER;
    Program(objects.program, fragment);
    glGenTextures(GLsizei(objects.textures.size()), objects.textures.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ALIGNMENT, 1);
    Texture(GL_TEXTURE0, objects.textures[1], GL_RGBA8, pass.target_width, pass.target_height,
        GL_RGBA, GL_UNSIGNED_BYTE, target.data());
    glGenFramebuffers(1, &objects.framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, objects.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, objects.textures[1], 0);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Incomplete native lens target");
    glDrawBuffer(GL_COLOR_ATTACHMENT0); glReadBuffer(GL_COLOR_ATTACHMENT0);
    Texture(GL_TEXTURE0, objects.textures[0], GL_RGBA8, pass.source_width, pass.source_height,
        GL_RGBA, GL_UNSIGNED_BYTE, source.data());
    if (pass.kind == LensPassKind::Mist)
        Texture(GL_TEXTURE1, objects.textures[2], GL_R8, 256, 256, GL_RED, GL_UNSIGNED_BYTE, pass.density.data());
    else if (pass.kind == LensPassKind::Remap)
        Texture(GL_TEXTURE1, objects.textures[2], GL_RG16UI, pass.remap_width, pass.remap_height,
            GL_RG_INTEGER, GL_UNSIGNED_SHORT, pass.remap.data());
    else if (pass.kind == LensPassKind::Overlay)
        Texture(GL_TEXTURE1, objects.textures[2], GL_R8, pass.overlay_width, pass.overlay_height,
            GL_RED, GL_UNSIGNED_BYTE, pass.overlay.data());
    if (!pass.palette_rgba.empty()) {
        Texture(GL_TEXTURE2, objects.textures[3], GL_RGBA8, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, pass.palette_rgba.data());
        Texture(GL_TEXTURE3, objects.textures[4], GL_R8, 512, 512, GL_RED, GL_UNSIGNED_BYTE, pass.rgb6_lookup.data());
    }
    if (!pass.fade.empty()) Texture(GL_TEXTURE4, objects.textures[5], GL_R8, 256, 64, GL_RED, GL_UNSIGNED_BYTE, pass.fade.data());
    glUseProgram(objects.program);
    const auto location = [&](const char* name) { return glGetUniformLocation(objects.program, name); };
    glUniform1i(location("u_scene"), 0);
    glUniform1i(location("u_mist"), 1); glUniform1i(location("u_remap"), 1); glUniform1i(location("u_overlay"), 1);
    glUniform1i(location("u_palette"), 2); glUniform1i(location("u_index_lookup"), 3); glUniform1i(location("u_fade_table"), 4);
    glUniform1f(location("u_src_x"), float(pass.source_x));
    glUniform2f(location("u_view_size"), float(pass.viewport_width), float(pass.viewport_height));
    glUniform2f(location("u_pos"), pass.mist_offsets[0], pass.mist_offsets[1]);
    glUniform2f(location("u_sec"), pass.mist_offsets[2], pass.mist_offsets[3]);
    glUniform1f(location("u_lightness"), float(pass.mist_lightness));
    glUniform1i(location("u_alpha"), pass.overlay_alpha);
    // Exact production quad topology and UVs (GLWorldViewRenderer.cpp).
    constexpr float quad[] = {-1,-1,0,0, 1,-1,1,0, 1,1,1,1,
        -1,-1,0,0, 1,1,1,1, -1,1,0,1};
    glGenVertexArrays(1, &objects.vao); glBindVertexArray(objects.vao);
    glGenBuffers(1, &objects.buffer); glBindBuffer(GL_ARRAY_BUFFER, objects.buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
    const GLint bottom = GLint(pass.target_height) - pass.destination_y - GLint(pass.viewport_height);
    glViewport(pass.destination_x, bottom, pass.viewport_width, pass.viewport_height);
    glEnable(GL_SCISSOR_TEST); glScissor(pass.destination_x, bottom, pass.viewport_width, pass.viewport_height);
    glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND); glDisable(GL_MULTISAMPLE); glDisable(GL_FRAMEBUFFER_SRGB);
    glEnable(GL_DITHER); // Production default; integer-valued operators still output alpha one.
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    CheckGL("Upload production lens resources");
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glReadPixels(0, 0, pass.target_width, pass.target_height, GL_RGBA, GL_UNSIGNED_BYTE, target.data());
    CheckGL("Production lens draw and readback");
    Image result{pass.target_width, pass.target_height, std::vector<uint8_t>(target.size())};
    const size_t stride = size_t(pass.target_width) * 4;
    for (uint32_t y = 0; y < pass.target_height; ++y)
        std::copy_n(target.data() + size_t(pass.target_height - 1 - y) * stride, stride, result.rgba.data() + size_t(y) * stride);
    return result;
}
}
