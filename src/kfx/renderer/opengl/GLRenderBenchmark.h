// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#ifndef KFX_GL_RENDER_BENCHMARK_H
#define KFX_GL_RENDER_BENCHMARK_H

#include "kfx/renderer/opengl/GLFunctions.h"
#include <array>
#include <chrono>
#include <string>
#include <vector>

// Owned exclusively by the GL thread. Timestamp pairs are collected later,
// without glFinish or waiting for a query result. Compatible with Tracy queries.
class GLRenderBenchmark {
public:
    std::string Begin(int width, int height, size_t terrain_vertices);
    void EndWork();
    void BeginSwap();
    void EndSwap();
    void Shutdown();
private:
    using Clock = std::chrono::steady_clock;
    struct Row {
        double cpu_ms = 0, gpu_ms = -1, interval_ms = 0, swap_ms = 0;
        int width = 0, height = 0;
        size_t terrain_vertices = 0;
    };
    struct Query { GLuint ids[2] = {}; int row = -1; };
    std::array<Query, 16> queries_{};
    std::vector<Row> rows_;
    Clock::time_point start_{}, previous_{}, swap_start_{};
    int scene_ = -1, warmup_ = 0, current_ = -1, query_ = -1;
    void Collect();
    std::string Finish();
};
#endif
