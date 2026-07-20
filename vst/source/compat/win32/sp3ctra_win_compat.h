/*
 * sp3ctra_win_compat.h — Windows portability core.
 *
 * The C core targets POSIX (pthread, unistd, sys/time, BSD sockets). On
 * Windows this directory is prepended to the include path and its POSIX
 * mirror headers (pthread.h, unistd.h, sys/time.h, arpa/inet.h, …) all
 * funnel here, so the C sources compile unchanged.
 *
 * Mapping choices:
 *   pthread_mutex_t → SRWLOCK           (static-initializable, non-recursive
 *                                        like the pthread default)
 *   pthread_cond_t  → CONDITION_VARIABLE (SleepConditionVariableSRW)
 *   pthread_t       → HANDLE            (_beginthreadex, join = Wait+Close)
 *   clock_gettime(CLOCK_MONOTONIC) → QueryPerformanceCounter
 *   gettimeofday    → GetSystemTimePreciseAsFileTime
 *   usleep          → Sleep (ms granularity — coarser than POSIX; the RT
 *                     paths use it only for idle backoff, not for pacing)
 *
 * Not implemented (unused by the codebase): pthread_attr_*, recursive
 * mutexes, pthread_cond_timedwait, semaphores.
 */
#pragma once

#ifndef _WIN32
#error "sp3ctra_win_compat.h is Windows-only; POSIX systems use the real headers"
#endif

/* winsock2 must precede windows.h (which would otherwise pull winsock 1). */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#include <direct.h>
#include <io.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>   /* UCRT: struct timespec */

#ifdef __cplusplus
extern "C" {
#endif

/* ── pthread types ─────────────────────────────────────────────────────── */

typedef HANDLE             pthread_t;
typedef SRWLOCK            pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef int                pthread_mutexattr_t;
typedef int                pthread_condattr_t;

#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
#define PTHREAD_COND_INITIALIZER  CONDITION_VARIABLE_INIT

/* ── mutex ─────────────────────────────────────────────────────────────── */

static inline int pthread_mutex_init(pthread_mutex_t *m,
                                     const pthread_mutexattr_t *attr) {
  (void)attr;
  InitializeSRWLock(m);
  return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *m) {
  (void)m; /* SRW locks need no teardown */
  return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *m) {
  AcquireSRWLockExclusive(m);
  return 0;
}

static inline int pthread_mutex_trylock(pthread_mutex_t *m) {
  return TryAcquireSRWLockExclusive(m) ? 0 : EBUSY;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *m) {
  ReleaseSRWLockExclusive(m);
  return 0;
}

/* ── condition variables ───────────────────────────────────────────────── */

static inline int pthread_cond_init(pthread_cond_t *c,
                                    const pthread_condattr_t *attr) {
  (void)attr;
  InitializeConditionVariable(c);
  return 0;
}

static inline int pthread_cond_destroy(pthread_cond_t *c) {
  (void)c;
  return 0;
}

static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
  return SleepConditionVariableSRW(c, m, INFINITE, 0) ? 0 : EINVAL;
}

static inline int pthread_cond_signal(pthread_cond_t *c) {
  WakeConditionVariable(c);
  return 0;
}

static inline int pthread_cond_broadcast(pthread_cond_t *c) {
  WakeAllConditionVariable(c);
  return 0;
}

/* ── threads ───────────────────────────────────────────────────────────── */

typedef struct sp3_win_thread_start {
  void *(*fn)(void *);
  void *arg;
} sp3_win_thread_start;

static inline unsigned __stdcall sp3_win_thread_tramp(void *p) {
  sp3_win_thread_start s = *(sp3_win_thread_start *)p;
  free(p);
  s.fn(s.arg);
  return 0;
}

static inline int pthread_create(pthread_t *thread, const void *attr,
                                 void *(*fn)(void *), void *arg) {
  (void)attr;
  sp3_win_thread_start *s =
      (sp3_win_thread_start *)malloc(sizeof(sp3_win_thread_start));
  if (s == NULL)
    return EAGAIN;
  s->fn = fn;
  s->arg = arg;
  uintptr_t h = _beginthreadex(NULL, 0, sp3_win_thread_tramp, s, 0, NULL);
  if (h == 0) {
    free(s);
    return EAGAIN;
  }
  *thread = (HANDLE)h;
  return 0;
}

static inline int pthread_join(pthread_t thread, void **retval) {
  if (retval != NULL)
    *retval = NULL;
  WaitForSingleObject(thread, INFINITE);
  CloseHandle(thread);
  return 0;
}

static inline int pthread_detach(pthread_t thread) {
  CloseHandle(thread);
  return 0;
}

static inline pthread_t pthread_self(void) {
  return GetCurrentThread(); /* pseudo-handle: valid for self-priority calls */
}

/* ── clocks ────────────────────────────────────────────────────────────── */

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

/* Unix epoch (1970) minus FILETIME epoch (1601), in 100 ns units. */
#define SP3_FILETIME_UNIX_EPOCH 116444736000000000ULL

static inline int sp3_clock_gettime(int clk, struct timespec *ts) {
  if (clk == CLOCK_MONOTONIC) {
    static LARGE_INTEGER freq; /* zero-init; QPF is constant after boot */
    LARGE_INTEGER now;
    if (freq.QuadPart == 0)
      QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    ts->tv_sec = (time_t)(now.QuadPart / freq.QuadPart);
    ts->tv_nsec =
        (long)((now.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
    return 0;
  }
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  ULARGE_INTEGER u;
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  unsigned long long t = u.QuadPart - SP3_FILETIME_UNIX_EPOCH;
  ts->tv_sec = (time_t)(t / 10000000ULL);
  ts->tv_nsec = (long)((t % 10000000ULL) * 100);
  return 0;
}
#define clock_gettime(clk, ts) sp3_clock_gettime((clk), (ts))

static inline int gettimeofday(struct timeval *tv, void *tz) {
  (void)tz;
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  ULARGE_INTEGER u;
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  unsigned long long t = u.QuadPart - SP3_FILETIME_UNIX_EPOCH;
  tv->tv_sec = (long)(t / 10000000ULL);
  tv->tv_usec = (long)((t % 10000000ULL) / 10);
  return 0;
}

/* ── unistd bits ───────────────────────────────────────────────────────── */

static inline int usleep(unsigned int usec) {
  Sleep(usec == 0 ? 0 : (usec + 999) / 1000);
  return 0;
}

static inline unsigned int sleep(unsigned int seconds) {
  Sleep(seconds * 1000);
  return 0;
}

/* sched_yield → SwitchToThread (yields only to ready threads, closest match) */
static inline int sched_yield(void) {
  SwitchToThread();
  return 0;
}

/* mlock/munlock → VirtualLock/VirtualUnlock (same intent: pin RT audio
 * buffers in physical RAM; NB Windows caps per-process locked pages unless
 * the working-set size is raised — failure is non-fatal, callers log it). */
static inline int mlock(const void *addr, size_t len) {
  return VirtualLock((LPVOID)addr, len) ? 0 : -1;
}

static inline int munlock(const void *addr, size_t len) {
  return VirtualUnlock((LPVOID)addr, len) ? 0 : -1;
}

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

#define isatty _isatty

/* POSIX mkdir(path, mode) → _mkdir(path). Callers pass '/' separators, which
 * every Windows path API accepts. */
#define mkdir(path, mode) _mkdir(path)

#ifdef __cplusplus
}
#endif
