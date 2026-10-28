#include "pre_inc.h"
#include "kfx/renderer/RenderThreadManager.h"
#include "kfx/renderer/RendererThread.h"
#include "kfx/profiling/KfxProfiling.h"
#ifdef _WIN32
#include <chrono>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "post_inc.h"

thread_local bool g_on_render_thread = false;

static void wait_for_render_thread(std::condition_variable& cv,
    std::unique_lock<std::mutex>& lock, const bool& completed)
{
#ifdef _WIN32
    if (RendererThread_IsGameThread())
    {
        // The GL driver can synchronously send a message to the window owner
        // while presenting or releasing its context. A plain wait on that
        // thread would prevent the render thread from ever completing.
        while (!cv.wait_for(lock, std::chrono::milliseconds(1), [&]{ return completed; }))
        {
            lock.unlock();
            MSG message;
            // Dispatch only sent native messages. Leave posted input and SDL
            // events for the normal game loop, and never hold m_mutex here.
            PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
            lock.lock();
        }
        return;
    }
#endif
    cv.wait(lock, [&]{ return completed; });
}

RenderThreadManager::~RenderThreadManager()
{
    if (m_active)
        Stop();
}

void RenderThreadManager::Start(Fn init_fn, Fn work_fn, Fn present_fn, Fn cleanup_fn)
{
    if (m_active)
        return;

    m_active      = true;
    m_initialized = false;
    m_quit        = false;
    m_work_ready  = false;
    m_work_done   = true;
    m_stopped     = false;

    m_thread = std::thread(
        &RenderThreadManager::ThreadProc, this,
        std::move(init_fn), std::move(work_fn), std::move(present_fn), std::move(cleanup_fn));

    std::unique_lock<std::mutex> lock(m_mutex);
    wait_for_render_thread(m_cv, lock, m_initialized);
}

void RenderThreadManager::WaitForCompletion()
{
    KFX_ZONE_COLOR("RenderThreadManager::WaitForCompletion", KFX_COLOR_SIMULATION);
    std::unique_lock<std::mutex> lock(m_mutex);
    wait_for_render_thread(m_cv, lock, m_work_done);
}

bool RenderThreadManager::IsWorkDone() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_work_done;
}

void RenderThreadManager::Signal()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_work_done  = false;
        m_work_ready = true;
    }
    m_cv.notify_one();
}

void RenderThreadManager::Stop()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_quit = true;
    }
    m_cv.notify_one();
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        wait_for_render_thread(m_cv, lock, m_stopped);
    }
    m_thread.join();
    m_active = false;
}

void RenderThreadManager::ThreadProc(Fn init_fn, Fn work_fn, Fn present_fn, Fn cleanup_fn)
{
    g_on_render_thread = true;
    RendererThread_RegisterRenderThread();
    init_fn();

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_initialized = true;
    }
    m_cv.notify_one();

    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]{ return m_work_ready || m_quit; });
            if (m_quit) break;
            m_work_ready = false;
        }

        {
            KFX_ZONE_COLOR("RenderThreadManager::work_fn", KFX_COLOR_RENDER_CPU);
            work_fn();
        }

        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_work_done = true;
        }
        m_cv.notify_one();
        present_fn();
    }

    cleanup_fn();
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_stopped = true;
    }
    m_cv.notify_one();
}
