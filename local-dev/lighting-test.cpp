// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Standalone Windows GPU regression check. Uses the production world shaders.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <glad/glad.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "kfx/renderer/LightmapSnapshot.h"
#include "kfx/renderer/WorldVertex.h"
#include "kfx/renderer/opengl/GLShaders.h"

static void require(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

static void* load_gl(const char* name)
{
    void* p = (void*)wglGetProcAddress(name);
    if (!p || p == (void*)1 || p == (void*)2 || p == (void*)3 || p == (void*)-1)
        p = (void*)GetProcAddress(GetModuleHandleA("opengl32.dll"), name);
    return p;
}

static GLuint shader(GLenum type, const char* source)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, nullptr);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[8192]; glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "%s\n", log);
    }
    require(ok, "production shader compilation");
    return s;
}

int main()
{
    // A smaller map must be copied row by row, with both edges extended.
    uint16_t source[] = {11, 12, 13, 21, 22, 23};
    uint16_t packed[20] = {};
    snapshot_lightmap(packed, 5, 4, source, 3, 2);
    const uint16_t expected[] = {11,12,13,13,13,21,22,23,23,23,21,22,23,23,23,21,22,23,23,23};
    require(std::equal(std::begin(packed), std::end(packed), std::begin(expected)), "map row stride and edge padding");
    uint16_t singleton = 77;
    snapshot_lightmap(packed, 5, 4, &singleton, 1, 1);
    require(std::all_of(std::begin(packed), std::end(packed), [](uint16_t v) { return v == 77; }), "one-cell lightmap");

    WNDCLASSA wc = {};
    wc.style = CS_OWNDC; wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "KeeperFXLightingTest";
    require(RegisterClassA(&wc), "register hidden test window");
    HWND window = CreateWindowA(wc.lpszClassName, "Lighting regression", WS_OVERLAPPEDWINDOW,
        0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
    require(SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd), "GL pixel format");
    HGLRC context = wglCreateContext(dc);
    require(context && wglMakeCurrent(dc, context), "hidden GPU context");
    require(gladLoadGLLoader(load_gl), "GL loader");
    std::printf("GPU: %s; GL: %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    GLuint program = glCreateProgram();
    glAttachShader(program, shader(GL_VERTEX_SHADER, WORLD_VERTEX_SHADER));
    glAttachShader(program, shader(GL_FRAGMENT_SHADER, WORLD_FRAGMENT_SHADER));
    glLinkProgram(program);
    GLint linked; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) { char log[8192]; glGetProgramInfoLog(program, sizeof(log), nullptr, log); std::fprintf(stderr, "%s\n", log); }
    require(linked, "world shader link");
    glUseProgram(program);
    auto integer = [program](const char* name, int value) { glUniform1i(glGetUniformLocation(program, name), value); };
    auto scalar = [program](const char* name, float value) { glUniform1f(glGetUniformLocation(program, name), value); };
    integer("u_tile_atlas", 0); integer("u_palette", 1); integer("u_lightmap", 2); integer("u_fade_table", 3);
    scalar("u_shade_scale", 1); scalar("u_shade_gamma", 1);
    GLuint textures[5]; glGenTextures(5, textures);
    auto bind = [&](int unit, GLenum target) {
        glActiveTexture(GL_TEXTURE0 + unit); glBindTexture(target, textures[unit]);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    bind(0, GL_TEXTURE_2D_ARRAY);
    std::array<unsigned char, 1024> tile; tile.fill(255);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8, 32, 32, 1, 0, GL_RED, GL_UNSIGNED_BYTE, tile.data());
    bind(1, GL_TEXTURE_2D);
    std::array<unsigned char, 1024> palette;
    for (int i = 0; i < 256; ++i) { palette[i*4] = palette[i*4+1] = palette[i*4+2] = i; palette[i*4+3] = 255; }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, palette.data());
    bind(3, GL_TEXTURE_2D);
    std::array<unsigned char, 16384> fade;
    for (int row = 0; row < 64; ++row) for (int i = 0; i < 256; ++i)
        fade[row*256+i] = std::min(255, (int)std::round(row * 255.0 / 32.0));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 64, 0, GL_RED, GL_UNSIGNED_BYTE, fade.data());
    bind(4, GL_TEXTURE_2D);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    GLuint fbo; glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[4], 0);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "offscreen framebuffer");
    glViewport(0, 0, 64, 64);
    GLuint vao, vbo; glGenVertexArrays(1, &vao); glBindVertexArray(vao); glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    const int sizes[] = {3,2,1,2,1,1,3,1};
    const size_t offsets[] = {offsetof(WorldVertex,x),offsetof(WorldVertex,u),offsetof(WorldVertex,shade),offsetof(WorldVertex,stl_x),
        offsetof(WorldVertex,camera_z),offsetof(WorldVertex,atlas_layer),offsetof(WorldVertex,wx),offsetof(WorldVertex,tile)};
    for (int i = 0; i < 8; ++i) { glEnableVertexAttribArray(i); glVertexAttribPointer(i, sizes[i], GL_FLOAT, GL_FALSE, sizeof(WorldVertex), (void*)offsets[i]); }
    int checks = 0;
    for (int darkness : {0, 1}) for (int filtering : {0, 1}) for (int mode : {0, 1})
    for (int scenario = 0; scenario < 6; ++scenario) {
        // Sample away from (0,0), which is deliberately dark. Unequal corners
        // distinguish bilinear light from the old triangle interpolation.
        uint16_t map[30] = {};
        map[3*5+2] = map[4*5+3] = 2048;
        map[3*5+3] = map[4*5+2] = 6144;
        if (scenario == 4) std::fill(std::begin(map), std::end(map), 0);
        uint16_t upload[64]; snapshot_lightmap(upload, 8, 8, map, 5, 6);
        bind(2, GL_TEXTURE_2D);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, 8, 8, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, upload);
        WorldVertex vertices[6] = {};
        const int corners[] = {0,1,2,0,2,3};
        const float xy[4][2] = {{0,0},{1,0},{1,1},{0,1}};
        for (int i = 0; i < 6; ++i) {
            const int c = corners[i]; const float x = xy[c][0], y = xy[c][1];
            auto& v = vertices[i]; v.x = x*2-1; v.y = y*2-1; v.camera_z = 1;
            v.stl_x = scenario == 2 ? -1 : x+2; v.stl_y = scenario == 2 ? -1 : y+3;
            v.shade = scenario == 3 ? 0 : scenario == 4 ? 0.4f : (c % 2 ? 0.75f : 0.25f) * (scenario == 1 ? 0.4f : 1.f);
            if (scenario == 5) { v.stl_x += 100; v.stl_y += 100; v.shade = 0.3f; }
        }
        integer("u_lighting_mode", mode); integer("u_darkness_mode", darkness); integer("u_tile_filter", filtering);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        std::array<unsigned char, 64*64*4> pixels;
        glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        require(glGetError() == GL_NO_ERROR, "GPU draw/readback");
        for (int y = 4; y < 64; y += 8) for (int x = 4; x < 64; x += 8) {
            const float u = (x+0.5f)/64, v = (y+0.5f)/64;
            float shade = mode == 1 && scenario != 2 ? 0.25f + 0.5f*(u+v-2*u*v) : 0.25f+0.5f*std::abs(u-v);
            if (scenario == 1) shade *= 0.4f;
            if (scenario == 3) shade = 0;
            if (scenario == 4) shade = 0.4f;
            if (scenario == 5) shade = 0.3f;
            int want = darkness ? (int)std::round(std::floor(shade*32+0.5f)*255/32) : (int)std::round(shade*255);
            int actual = pixels[(y*64+x)*4];
            if (std::abs(want-actual) > 1) {
                std::fprintf(stderr, "mode=%d darkness=%d filter=%d scenario=%d pixel=%d,%d expected=%d actual=%d\n", mode,darkness,filtering,scenario,x,y,want,actual);
                require(false, "lighting pixel regression");
            }
            ++checks;
        }
    }
    std::printf("PASS: row stride, edge padding, shader compile/link, %d GPU pixel checks (modern/original, face shade, unrevealed darkness, missing coords, zero light, bounds, palette/linear, both texture filters).\n", checks);
    wglMakeCurrent(nullptr, nullptr); wglDeleteContext(context); ReleaseDC(window, dc); DestroyWindow(window);
    return 0;
}
