#include <stddef.h>
#include <stdbool.h>
#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#elif defined(_WIN32)
#include <windows.h>
#include <limits.h>
#else
#include <semaphore.h>
#include <time.h>
#endif
extern volatile int g_vst_callback_consumed_buffer;

// Process-lifetime semaphore: created before the audio threads start, never
// destroyed underneath a callback. The flag coalesces redundant notifications.
#if defined(__APPLE__)
static dispatch_semaphore_t consumedSemaphore = NULL;
#elif defined(_WIN32)
static HANDLE consumedSemaphore = NULL;
#else
static sem_t consumedSemaphore;
static bool consumedSemaphoreInitialized = false;
#endif

void luxstral_init_callback_sync(void) {
#if defined(__APPLE__)
    if (!consumedSemaphore) consumedSemaphore = dispatch_semaphore_create(0);
#elif defined(_WIN32)
    if (!consumedSemaphore) consumedSemaphore = CreateSemaphoreW(NULL, 0, LONG_MAX, NULL);
#else
    if (!consumedSemaphoreInitialized)
        consumedSemaphoreInitialized = sem_init(&consumedSemaphore, 0, 0) == 0;
#endif
    __atomic_store_n(&g_vst_callback_consumed_buffer, 1, __ATOMIC_RELEASE);
}

void luxstral_signal_buffer_consumed(void) {
    if (__atomic_exchange_n(&g_vst_callback_consumed_buffer, 1, __ATOMIC_ACQ_REL) != 0)
        return;
#if defined(__APPLE__)
    dispatch_semaphore_signal(consumedSemaphore);
#elif defined(_WIN32)
    ReleaseSemaphore(consumedSemaphore, 1, NULL);
#else
    sem_post(&consumedSemaphore);
#endif
}

void luxstral_cleanup_callback_sync(void) {
    luxstral_signal_buffer_consumed();
}

// 1 means permission to render; 0 means timeout/interruption, never consumption.
// Tokens are hints; the atomic exchange is the ownership transfer. A token
// left by the fast path is harmless and is drained by the next wait.
int luxstral_wait_for_buffer_consumed(void) {
    for (;;) {
        if (__atomic_exchange_n(&g_vst_callback_consumed_buffer, 0, __ATOMIC_ACQ_REL)) {
            // Drain the matching hint even on the fast path, so tokens cannot
            // accumulate when the consumer consistently runs ahead of us.
#if defined(__APPLE__)
            dispatch_semaphore_wait(consumedSemaphore, DISPATCH_TIME_NOW);
#elif defined(_WIN32)
            WaitForSingleObject(consumedSemaphore, 0);
#else
            sem_trywait(&consumedSemaphore);
#endif
            return 1;
        }
#if defined(__APPLE__)
        if (dispatch_semaphore_wait(consumedSemaphore,
                dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC)) != 0)
            return 0;
#elif defined(_WIN32)
        if (WaitForSingleObject(consumedSemaphore, 50) != WAIT_OBJECT_0) return 0;
#else
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 50000000;
        if (deadline.tv_nsec >= 1000000000) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000;
        }
        if (sem_timedwait(&consumedSemaphore, &deadline) != 0) return 0;
#endif
    }
}
