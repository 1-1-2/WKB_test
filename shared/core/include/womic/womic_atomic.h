/*
 * womic_atomic.h -- 三态（内核态 / Windows 用户态 / GCC-Clang）内存序抽象层。
 *
 * 本文件是 womic-open 项目中唯一允许出现平台分支的原子操作封装，被以下三方共用：
 *   1) Linux 单元测试（GCC/Clang, stdatomic.h）
 *   2) Windows 用户态服务端（MSVC, Win32 Interlocked* Acquire/Release 变体）
 *   3) Windows 内核驱动（MSVC + WDK, wdm.h 的 ReadAcquire / WriteRelease / MemoryBarrier）
 *
 * 设计约束：
 *   - 纯 C99，不使用任何 C++ 特性；
 *   - 只提供 32 位无符号整型的 acquire-load / release-store / 全栅栏三种能力，
 *     因为共享结构体里的序号（head/tail/state）全部是 uint32_t；
 *   - 内核态不允许链接 libc，因此只能使用 WDK 提供的内联屏障原语；
 *   - 三个分支的语义必须等价：
 *       LOAD_ACQUIRE  : 其后的读写不得被重排到它之前；
 *       STORE_RELEASE : 其前的读写不得被重排到它之后；
 *       FENCE_FULL    : 双向全序栅栏（seq_cst）。
 */
#ifndef WOMIC_ATOMIC_H
#define WOMIC_ATOMIC_H

#include <stdint.h>

#if defined(_WIN32) && defined(_KERNEL_MODE)
/* ------------------------------------------------------------------ *
 * Windows 内核态（WDK）。wdm.h 提供 ReadAcquire/WriteRelease/MemoryBarrier，
 * 它们在 x86/x64 上是编译器屏障，在 ARM64 上展开为真实的 ldar/stlr/dmb。
 * ------------------------------------------------------------------ */
#  include <wdm.h>
#  define WOMIC_LOAD_ACQUIRE_U32(p)    ((uint32_t)ReadAcquire((LONG volatile *)(p)))
#  define WOMIC_STORE_RELEASE_U32(p,v) WriteRelease((LONG volatile *)(p), (LONG)(v))
#  define WOMIC_FENCE_FULL()           MemoryBarrier()

#elif defined(_WIN32)
/* ------------------------------------------------------------------ *
 * Windows 用户态（MSVC + Win32）。
 *
 * InterlockedCompareExchangeAcquire(dest, 0, 0) 在语义上等价于“带 acquire 语义的
 * 读取”：它返回 *dest 的当前值且不修改它（比较值与交换值相同，即使比较失败也返回
 * 当前值），同时具备 acquire 屏障语义。这是 Win32 上实现 load-acquire 的标准写法。
 *
 * InterlockedExchangeRelease(dest, v) 写入 v 并返回旧值，具备 release 屏障语义。
 * 用 (void) 显式丢弃返回值，避免 C4996/未使用值告警。
 * ------------------------------------------------------------------ */
#  include <windows.h>
#  define WOMIC_LOAD_ACQUIRE_U32(p)    ((uint32_t)InterlockedCompareExchangeAcquire((volatile LONG *)(p), 0L, 0L))
#  define WOMIC_STORE_RELEASE_U32(p,v) ((void)InterlockedExchangeRelease((volatile LONG *)(p), (LONG)(uint32_t)(v)))
#  define WOMIC_FENCE_FULL()           MemoryBarrier()

#elif defined(__cplusplus)
/* ------------------------------------------------------------------ *
 * C++ 翻译单元（仅用于让头文件可被 C++ 测试代码包含并参与类型检查）。
 * C++ 没有 _Atomic 关键字，改用 GCC/Clang 的 __atomic_* 内建函数，
 * 语义与 C11 stdatomic 一一对应。真正的并发读写仍发生在 C 语言实现里。
 * ------------------------------------------------------------------ */
#  define WOMIC_LOAD_ACQUIRE_U32(p)    ((uint32_t)__atomic_load_n((const volatile uint32_t *)(p), __ATOMIC_ACQUIRE))
#  define WOMIC_STORE_RELEASE_U32(p,v) ((void)__atomic_store_n((volatile uint32_t *)(p), (uint32_t)(v), __ATOMIC_RELEASE))
#  define WOMIC_FENCE_FULL()           __atomic_thread_fence(__ATOMIC_SEQ_CST)

#else
/* ------------------------------------------------------------------ *
 * GCC / Clang（Linux 单元测试、以及未来可能的 POSIX 侧移植）。
 * ------------------------------------------------------------------ */
#  include <stdatomic.h>
#  define WOMIC_LOAD_ACQUIRE_U32(p)    ((uint32_t)atomic_load_explicit((_Atomic uint32_t *)(p), memory_order_acquire))
#  define WOMIC_STORE_RELEASE_U32(p,v) atomic_store_explicit((_Atomic uint32_t *)(p), (uint32_t)(v), memory_order_release)
#  define WOMIC_FENCE_FULL()           atomic_thread_fence(memory_order_seq_cst)
#endif

/*
 * 便捷封装：统计计数器使用 relaxed 语义（各自独占，只需保证不撕裂）。
 * 内核态下 InterlockedExchangeAdd / InterlockedIncrement 均可用。
 */
#if defined(_WIN32)
#  if defined(_KERNEL_MODE)
#    define WOMIC_ADD_RELAXED_U64(p, v) ((void)InterlockedExchangeAdd64((LONG64 volatile *)(p), (LONG64)(v)))
#  else
#    define WOMIC_ADD_RELAXED_U64(p, v) ((void)InterlockedExchangeAdd64((LONG64 volatile *)(p), (LONG64)(v)))
#  endif
#elif defined(__cplusplus)
#  define WOMIC_ADD_RELAXED_U64(p, v)  ((void)__atomic_add_fetch((volatile uint64_t *)(p), (uint64_t)(v), __ATOMIC_RELAXED))
#else
#  define WOMIC_ADD_RELAXED_U64(p, v)  ((void)atomic_fetch_add_explicit((_Atomic uint64_t *)(p), (uint64_t)(v), memory_order_relaxed))
#endif

#endif /* WOMIC_ATOMIC_H */
