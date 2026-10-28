// SPDX-License-Identifier: GPL-2.0-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "kfx/renderer/RenderThreadManager.h"
#include "kfx/renderer/RendererThread.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
constexpr UINT sent_message = WM_APP + 17;
constexpr LRESULT sent_reply = 0x4b4658;
RenderThreadManager* manager = nullptr;
DWORD owner_thread = 0;
unsigned delivered = 0;
unsigned dispatched_input = 0;
bool wrong_thread = false;
std::atomic<bool> send_failed{false};

[[noreturn]] void Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::fflush(stderr);
    // Do not enter a blocked manager destructor after a failed assertion.
    ExitProcess(1);
}

void Require(bool condition, const char* message)
{
    if (!condition) Fail(message);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == sent_message) {
        wrong_thread |= GetCurrentThreadId() != owner_thread;
        // A wait must release the manager mutex before dispatching a message.
        // The runner watchdog catches a self-deadlock if it does not.
        (void)manager->IsWorkDone();
        ++delivered;
        return sent_reply;
    }
    if (message == WM_KEYDOWN) {
        ++dispatched_input;
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void SendToOwner(HWND window, const char* phase)
{
    std::printf("send begin: %s\n", phase);
    std::fflush(stdout);
    DWORD_PTR reply = 0;
    const LRESULT completed = SendMessageTimeoutW(window, sent_message, 0, 0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &reply);
    if (!completed || reply != static_cast<DWORD_PTR>(sent_reply)) {
        send_failed.store(true);
        std::fprintf(stderr, "sent message timed out or failed: %s (error=%lu)\n",
            phase, GetLastError());
        std::fflush(stderr);
    }
    std::printf("send returned: %s delivered=%d\n", phase, completed != 0);
    std::fflush(stdout);
}

void PostInput(HWND window)
{
    Require(PostMessageW(window, WM_KEYDOWN, VK_F12, 0) != 0, "PostMessage failed");
}

void CheckDeliveryAndInput(HWND window)
{
    // Check delivery before PeekMessage: the assertion must not accidentally
    // unblock an outstanding send that the tested wait failed to service.
    Require(!send_failed.load(), "sent message did not complete during the manager wait");
    Require(delivered == 1, "expected exactly one synchronous message delivery");
    Require(!wrong_thread, "window message dispatched on the wrong thread");
    Require(dispatched_input == 0, "manager wait consumed posted input");
    MSG input{};
    Require(PeekMessageW(&input, window, WM_KEYDOWN, WM_KEYDOWN, PM_REMOVE) != 0,
        "manager wait removed posted input from the queue");
    Require(input.message == WM_KEYDOWN && input.wParam == VK_F12,
        "posted input changed during manager wait");
}

void WaitForPresentEntry(const std::atomic<bool>& entered)
{
    const ULONGLONG deadline = GetTickCount64() + 1000;
    while (!entered.load() && GetTickCount64() < deadline) Sleep(1);
    Require(entered.load(), "render callback did not reach presentation");
}
}

int main(int argc, char** argv)
{
    Require(argc == 2, "usage: render-thread-test.exe init|work|present|present-stop|cleanup|restart");
    const char* scenario = argv[1];
    owner_thread = GetCurrentThreadId();
    RendererThread_RegisterGameThread();
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"KeeperFXRenderThreadWaitTest";
    Require(RegisterClassW(&window_class) != 0, "RegisterClass failed");
    HWND window = CreateWindowExW(0, window_class.lpszClassName, L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, window_class.hInstance, nullptr);
    Require(window != nullptr, "CreateWindow failed");

    RenderThreadManager tested;
    manager = &tested;
    const auto noop = [] {};
    if (std::strcmp(scenario, "init") == 0 || std::strcmp(scenario, "restart") == 0) {
        if (std::strcmp(scenario, "restart") == 0) {
            tested.Start(noop, noop, noop, noop);
            tested.Signal();
            tested.WaitForCompletion();
            tested.Stop();
        }
        PostInput(window);
        tested.Start([&] { SendToOwner(window, scenario); }, noop, noop, noop);
        CheckDeliveryAndInput(window);
        tested.Stop();
    } else if (std::strcmp(scenario, "work") == 0) {
        tested.Start(noop, [&] { SendToOwner(window, "work"); }, noop, noop);
        PostInput(window);
        tested.Signal();
        tested.WaitForCompletion();
        CheckDeliveryAndInput(window);
        tested.Stop();
    } else if (std::strcmp(scenario, "present") == 0 || std::strcmp(scenario, "present-stop") == 0) {
        std::atomic<unsigned> work_count{0};
        std::atomic<unsigned> present_count{0};
        std::atomic<bool> entered{false};
        tested.Start(noop, [&] { ++work_count; }, [&] {
            if (++present_count == 1) {
                entered.store(true);
                SendToOwner(window, "present");
            }
        }, noop);
        PostInput(window);
        tested.Signal();
        tested.WaitForCompletion();
        WaitForPresentEntry(entered);
        if (std::strcmp(scenario, "present") == 0) {
            // Completion releases the frame data before presentation finishes.
            // Queue a second frame while the first present is waiting on HWND.
            tested.Signal();
            tested.WaitForCompletion();
            Require(work_count.load() == 2, "next frame did not finish its work");
            CheckDeliveryAndInput(window);
            tested.Stop();
        } else {
            tested.Stop();
            CheckDeliveryAndInput(window);
        }
    } else if (std::strcmp(scenario, "cleanup") == 0) {
        tested.Start(noop, noop, noop, [&] { SendToOwner(window, "cleanup"); });
        PostInput(window);
        tested.Stop();
        CheckDeliveryAndInput(window);
    } else {
        Fail("unknown scenario");
    }
    Require(!tested.IsActive(), "manager remained active after Stop");
    DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    std::printf("{\"passed\":true,\"scenario\":\"%s\",\"sentMessages\":%u,\"postedInputPreserved\":true}\n",
        scenario, delivered);
    return 0;
}
