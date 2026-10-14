// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/opengl/GLRenderBenchmark.h"
#include "kfx/renderer/RenderBenchmark.h"
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iomanip>

namespace {
constexpr int warmup_frames = 120;
constexpr size_t sample_frames = 360;
const std::string& output_directory()
{
    static const std::string path = RenderBenchmarkOutputDirectory();
    return path;
}
}

void GLRenderBenchmark::Collect()
{
    for (auto& q : queries_) {
        if (q.row < 0) continue;
        GLint available = GL_FALSE;
        glGetQueryObjectiv(q.ids[1], GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available) continue;
        GLuint64 start = 0, end = 0;
        glGetQueryObjectui64v(q.ids[0], GL_QUERY_RESULT, &start);
        glGetQueryObjectui64v(q.ids[1], GL_QUERY_RESULT, &end);
        if (end >= start) rows_[q.row].gpu_ms = double(end - start) / 1e6;
        q.row = -1;
    }
}

std::string GLRenderBenchmark::Begin(int width, int height, size_t terrain_vertices)
{
    if (!RenderBenchmarkEnabled()) return {};
    current_ = query_ = -1;
    const int requested = RenderBenchmarkTakeRequest(0);
    if (requested >= 0) {
        scene_ = requested;
        warmup_ = warmup_frames;
        rows_.clear();
        rows_.reserve(sample_frames);
    }
    if (scene_ < 0) return {};
    Collect();
    if (rows_.size() == sample_frames) {
        for (const auto& q : queries_) if (q.row >= 0) return {};
        return Finish();
    }
    const auto now = Clock::now();
    const double interval = std::chrono::duration<double, std::milli>(now - previous_).count();
    previous_ = now;
    if (warmup_-- > 0) return {};
    current_ = int(rows_.size());
    rows_.push_back({0, -1, interval, 0, width, height, terrain_vertices});
    for (size_t i = 0; i < queries_.size(); ++i) {
        auto& q = queries_[i];
        if (q.row >= 0) continue;
        if (!q.ids[0]) glGenQueries(2, q.ids);
        query_ = int(i);
        q.row = current_;
        glQueryCounter(q.ids[0], GL_TIMESTAMP);
        break;
    }
    start_ = Clock::now();
    return {};
}

void GLRenderBenchmark::EndWork()
{
    if (current_ < 0) return;
    rows_[current_].cpu_ms = std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
    if (query_ >= 0) glQueryCounter(queries_[query_].ids[1], GL_TIMESTAMP);
}
void GLRenderBenchmark::BeginSwap() { if (current_ >= 0) swap_start_ = Clock::now(); }
void GLRenderBenchmark::EndSwap()
{
    if (current_ >= 0)
        rows_[current_].swap_ms = std::chrono::duration<double, std::milli>(Clock::now() - swap_start_).count();
}
std::string GLRenderBenchmark::Finish()
{
    const std::string stem = output_directory() + "/scene-" + std::to_string(scene_);
    std::ofstream out(stem + ".csv");
    out << "sample,render_cpu_ms,gpu_ms,frame_interval_ms,swap_cpu_ms,width,height,terrain_vertices\n";
    out << std::fixed << std::setprecision(6);
    for (size_t i = 0; i < rows_.size(); ++i) {
        const auto& r = rows_[i];
        out << i << ',' << r.cpu_ms << ',' << r.gpu_ms << ',' << r.interval_ms << ',' << r.swap_ms
            << ',' << r.width << ',' << r.height << ',' << r.terrain_vertices << '\n';
    }
    out.close();
    scene_ = -1;
    RenderBenchmarkComplete();
    return stem + ".png";
}
void GLRenderBenchmark::Shutdown()
{
    for (auto& q : queries_) {
        if (q.ids[0]) glDeleteQueries(2, q.ids);
        q = {};
    }
    current_ = scene_ = query_ = -1;
    RenderBenchmarkComplete();
}
