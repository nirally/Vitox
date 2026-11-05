// Vitox SDL core shim implementation: std::thread + std::counting_semaphore (UWP-safe).
// This is a core-only stand-in for exactly the symbols declared in sdl-shim/SDL3/*.h,
// not an SDL replacement for frontend/input/audio.
#include <SDL3/SDL_thread.h>

#include "vitox_sdl_shim_report.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <semaphore>
#include <string>
#include <thread>

struct SDL_Thread {
    std::thread thread;
};

struct SDL_Semaphore {
    explicit SDL_Semaphore(std::uint32_t initial)
        : sem(static_cast<std::ptrdiff_t>(initial)) {}
    std::counting_semaphore<> sem;
};

namespace {

std::atomic<unsigned> g_create_thread{ 0 }, g_detach_thread{ 0 }, g_threads_named{ 0 };
std::atomic<unsigned> g_create_sem{ 0 }, g_signal_sem{ 0 }, g_wait_sem{ 0 }, g_destroy_sem{ 0 };

// Debug thread name, best effort (resolved at runtime; never a static import).
void name_current_thread(const char *name) {
    if (!name || !*name)
        return;
    using SetDesc = HRESULT(WINAPI *)(HANDLE, PCWSTR);
    static const auto set_desc = [] {
        HMODULE k = GetModuleHandleW(L"kernelbase.dll");
        if (!k)
            k = GetModuleHandleW(L"kernel32.dll");
        return k ? reinterpret_cast<SetDesc>(GetProcAddress(k, "SetThreadDescription")) : nullptr;
    }();
    if (!set_desc)
        return;
    const int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    if (n <= 0)
        return;
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name, -1, w.data(), n);
    if (SUCCEEDED(set_desc(GetCurrentThread(), w.c_str())))
        ++g_threads_named;
}

} // namespace

extern "C" SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data) {
    ++g_create_thread;
    auto *t = new SDL_Thread;
    std::string thread_name = name ? name : "";
    t->thread = std::thread([fn, data, thread_name] {
        name_current_thread(thread_name.c_str());
        fn(data);
    });
    return t;
}

extern "C" void SDL_DetachThread(SDL_Thread *thread) {
    ++g_detach_thread;
    if (!thread)
        return;
    if (thread->thread.joinable())
        thread->thread.detach();
    delete thread;
}

extern "C" SDL_Semaphore *SDL_CreateSemaphore(std::uint32_t initial_value) {
    ++g_create_sem;
    return new SDL_Semaphore(initial_value);
}

extern "C" void SDL_SignalSemaphore(SDL_Semaphore *sem) {
    ++g_signal_sem;
    sem->sem.release();
}

extern "C" void SDL_WaitSemaphore(SDL_Semaphore *sem) {
    ++g_wait_sem;
    sem->sem.acquire();
}

extern "C" void SDL_DestroySemaphore(SDL_Semaphore *sem) {
    ++g_destroy_sem;
    delete sem;
}

extern "C" VitoxSdlShimReport vitox_sdl_shim_report() {
    return { g_create_thread.load(), g_detach_thread.load(), g_threads_named.load(), g_create_sem.load(),
        g_signal_sem.load(), g_wait_sem.load(), g_destroy_sem.load() };
}
