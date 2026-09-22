#include "synth_work_dispatch.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <errno.h>
#if defined(__APPLE__)
#include <dispatch/dispatch.h>
typedef dispatch_semaphore_t WorkSemaphore;
static int sem_create(WorkSemaphore* s) { *s = dispatch_semaphore_create(0); return *s ? 0 : -1; }
static void sem_signal(WorkSemaphore* s) { dispatch_semaphore_signal(*s); }
static void sem_await(WorkSemaphore* s) { dispatch_semaphore_wait(*s, DISPATCH_TIME_FOREVER); }
static void sem_dispose(WorkSemaphore* s) { dispatch_release(*s); }
#elif defined(_WIN32)
#include <windows.h>
typedef HANDLE WorkSemaphore;
static int sem_create(WorkSemaphore* s) { *s = CreateSemaphoreW(NULL, 0, 1, NULL); return *s ? 0 : -1; }
static void sem_signal(WorkSemaphore* s) { ReleaseSemaphore(*s, 1, NULL); }
static void sem_await(WorkSemaphore* s) { WaitForSingleObject(*s, INFINITE); }
static void sem_dispose(WorkSemaphore* s) { CloseHandle(*s); }
#else
#include <semaphore.h>
typedef sem_t WorkSemaphore;
static int sem_create(WorkSemaphore* s) { return sem_init(s, 0, 0); }
static void sem_signal(WorkSemaphore* s) { sem_post(s); }
static void sem_await(WorkSemaphore* s) { while (sem_wait(s) != 0 && errno == EINTR) {} }
static void sem_dispose(WorkSemaphore* s) { sem_destroy(s); }
#endif
#define WORK_AUX_MAX 15
struct SynthWorkDispatch {
    WorkSemaphore start[WORK_AUX_MAX], done;
    int auxiliaries;
    atomic_int stopped, remaining;
    atomic_uint generation;
};
SynthWorkDispatch* synth_work_dispatch_create(int auxiliaries) {
    if (auxiliaries < 0 || auxiliaries > WORK_AUX_MAX) return NULL;
    SynthWorkDispatch* d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    if (sem_create(&d->done) != 0) { free(d); return NULL; }
    for (int i = 0; i < auxiliaries; ++i) {
        if (sem_create(&d->start[i]) != 0) {
            for (int j = 0; j < i; ++j) sem_dispose(&d->start[j]);
            sem_dispose(&d->done); free(d); return NULL;
        }
    }
    d->auxiliaries = auxiliaries;
    return d;
}
void synth_work_dispatch_begin(SynthWorkDispatch* d) {
    atomic_store_explicit(&d->remaining, d->auxiliaries, memory_order_relaxed);
    atomic_fetch_add_explicit(&d->generation, 1, memory_order_release);
    for (int i = 0; i < d->auxiliaries; ++i) sem_signal(&d->start[i]);
}
int synth_work_dispatch_wait(SynthWorkDispatch* d, int auxiliary) {
    sem_await(&d->start[auxiliary]);
    if (atomic_load_explicit(&d->stopped, memory_order_acquire)) return 0;
    (void)atomic_load_explicit(&d->generation, memory_order_acquire);
    return 1;
}
void synth_work_dispatch_complete(SynthWorkDispatch* d) {
    /* The release sequence collects every worker's writes. Only the last
     * worker signals; there is no second rendezvous among the workers. */
    if (atomic_fetch_sub_explicit(&d->remaining, 1, memory_order_acq_rel) == 1)
        sem_signal(&d->done);
}
void synth_work_dispatch_finish(SynthWorkDispatch* d) {
    if (d->auxiliaries) sem_await(&d->done);
    (void)atomic_load_explicit(&d->remaining, memory_order_acquire);
}
void synth_work_dispatch_stop(SynthWorkDispatch* d) {
    if (!d || atomic_exchange_explicit(&d->stopped, 1, memory_order_acq_rel)) return;
    for (int i = 0; i < d->auxiliaries; ++i) sem_signal(&d->start[i]);
}
void synth_work_dispatch_destroy(SynthWorkDispatch* d) {
    if (!d) return;
    for (int i = 0; i < d->auxiliaries; ++i) sem_dispose(&d->start[i]);
    sem_dispose(&d->done); free(d);
}
