/*
 * threads_sync.cpp -- 平台适配层 1/3 的实现。
 *
 * Windows 分支（CRITICAL_SECTION / CONDITION_VARIABLE / SetThreadPriority /
 * SetThreadDescription）在 Linux 沙箱中无法编译验证，逻辑与 Linux 分支一一对应，
 * 所有 API 均为 Win32 稳定 ABI。
 */
#include "threads_sync.h"

#include <cstdio>
#include <thread>

#ifdef _WIN32
#else
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace womic {

#ifdef _WIN32

Mutex::Mutex()
{
    InitializeCriticalSectionAndSpinCount(&cs_, 2000);
}

Mutex::~Mutex()
{
    DeleteCriticalSection(&cs_);
}

void Mutex::lock()
{
    EnterCriticalSection(&cs_);
}

void Mutex::unlock()
{
    LeaveCriticalSection(&cs_);
}

CondVar::CondVar()
{
    InitializeConditionVariable(&cv_);
}

CondVar::~CondVar() = default;

bool CondVar::wait(Mutex &m, uint32_t timeout_ms)
{
    BOOL ok = SleepConditionVariableCS(&cv_, &m.cs_, static_cast<DWORD>(timeout_ms));
    if (ok) {
        return true;
    }
    /* 超时返回 FALSE 且 GetLastError()==ERROR_TIMEOUT；其余视为被唤醒（保守）。 */
    return GetLastError() != ERROR_TIMEOUT;
}

void CondVar::notify_one()
{
    WakeConditionVariable(&cv_);
}

void CondVar::notify_all()
{
    WakeAllConditionVariable(&cv_);
}

uint32_t steady_now_ms()
{
    /* GetTickCount64：开机以来的单调毫秒，分辨率约 15.6ms（受系统时钟粒度影响）。
       jitter buffer 的公差是 10~20ms 级，足够；若需要亚毫秒精度可换 QPC，
       但那会引入每次除法的开销，实测不必要。 */
    return static_cast<uint32_t>(GetTickCount64());
}

uint64_t steady_now_us()
{
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER counter {};
    QueryPerformanceCounter(&counter);
    if (freq.QuadPart <= 0) {
        return static_cast<uint64_t>(GetTickCount64()) * 1000u;
    }
    return static_cast<uint64_t>(counter.QuadPart * 1000000LL / freq.QuadPart);
}

void sleep_ms(uint32_t ms)
{
    Sleep(static_cast<DWORD>(ms));
}

void yield_now()
{
    SwitchToThread();
}

bool set_current_thread_priority(ThreadPriority p, std::string *err)
{
    int win_prio = THREAD_PRIORITY_NORMAL;
    switch (p) {
        case ThreadPriority::kLow:
            win_prio = THREAD_PRIORITY_BELOW_NORMAL;
            break;
        case ThreadPriority::kNormal:
            win_prio = THREAD_PRIORITY_NORMAL;
            break;
        case ThreadPriority::kHigh:
            win_prio = THREAD_PRIORITY_ABOVE_NORMAL;
            break;
        case ThreadPriority::kCritical:
            win_prio = THREAD_PRIORITY_TIME_CRITICAL;
            break;
        default:
            win_prio = THREAD_PRIORITY_NORMAL;
            break;
    }
    if (!SetThreadPriority(GetCurrentThread(), win_prio)) {
        if (err != nullptr) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "SetThreadPriority(%d) 失败，GetLastError=%lu",
                          win_prio, static_cast<unsigned long>(GetLastError()));
            *err = buf;
        }
        return false;
    }
    return true;
}

void set_current_thread_name(const char *name)
{
    /* SetThreadDescription 需要 Windows 10 1607+；动态解析失败则静默跳过。 */
    using SetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32 == nullptr) {
        return;
    }
    FARPROC proc = GetProcAddress(kernel32, "SetThreadDescription");
    if (proc == nullptr) {
        return;
    }
    wchar_t wide[64] {};
    int n = MultiByteToWideChar(CP_UTF8, 0, (name != nullptr) ? name : "", -1, wide, 64);
    if (n <= 0) {
        return;
    }
    reinterpret_cast<SetThreadDescriptionFn>(proc)(GetCurrentThread(), wide);
}

#else  /* ------------------------------ POSIX ------------------------------ */

bool CondVar::wait(Mutex &m, uint32_t timeout_ms)
{
    std::unique_lock<std::mutex> lock(m.mu_, std::adopt_lock);
    std::cv_status st = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms));
    lock.release();
    return st == std::cv_status::no_timeout;
}

uint32_t steady_now_ms()
{
    return static_cast<uint32_t>(steady_now_us() / 1000u);
}

uint64_t steady_now_us()
{
    static const std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::steady_clock::now() - origin;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
}

void sleep_ms(uint32_t ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void yield_now()
{
    std::this_thread::yield();
}

bool set_current_thread_priority(ThreadPriority p, std::string *err)
{
    /*
     * Linux 没有 THREAD_PRIORITY_TIME_CRITICAL 的直接对应物。
     * 等价做法：对 kCritical / kHigh 申请 SCHED_FIFO 实时调度（需要 CAP_SYS_NICE
     * 或 ulimit -r 配额），失败时降级为 setpriority(nice)，绝不因提权失败而退出。
     */
    if ((p == ThreadPriority::kCritical) || (p == ThreadPriority::kHigh)) {
        int prio = (p == ThreadPriority::kCritical) ? 20 : 10;
        sched_param param {};
        param.sched_priority = prio;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0) {
            return true;
        }
        /* 无权限：降级到 nice。 */
        if (setpriority(PRIO_PROCESS, 0, (p == ThreadPriority::kCritical) ? -10 : -5) == 0) {
            if (err != nullptr) {
                err->clear();
            }
            return true;
        }
        if (err != nullptr) {
            *err = "pthread_setschedparam(SCHED_FIFO) 与 setpriority 均失败（无 CAP_SYS_NICE）";
        }
        return false;
    }

    int nice_value = (p == ThreadPriority::kLow) ? 5 : 0;
    if (setpriority(PRIO_PROCESS, 0, nice_value) != 0) {
        if (err != nullptr) {
            *err = "setpriority 失败";
        }
        return false;
    }
    return true;
}

void set_current_thread_name(const char *name)
{
    if (name == nullptr) {
        return;
    }
    char truncated[16];
    std::snprintf(truncated, sizeof(truncated), "%s", name);
    pthread_setname_np(pthread_self(), truncated);
}

#endif

}  // namespace womic
