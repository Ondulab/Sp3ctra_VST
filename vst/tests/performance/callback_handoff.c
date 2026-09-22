#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>
volatile int g_vst_callback_consumed_buffer = 1;
void luxstral_init_callback_sync(void);
void luxstral_signal_buffer_consumed(void);
int luxstral_wait_for_buffer_consumed(void);
static atomic_int produced;
static void* producer(void* unused) {
    (void)unused;
    for (int n = 1; n <= 1000; ++n) {
        while (!luxstral_wait_for_buffer_consumed()) {}
        atomic_store_explicit(&produced, n, memory_order_release);
    }
    return NULL;
}
int main(void) {
    luxstral_init_callback_sync();
    assert(luxstral_wait_for_buffer_consumed() == 1);
    assert(luxstral_wait_for_buffer_consumed() == 0);
    // Thousands of duplicate signals must still authorize a single render.
    for (int n = 0; n < 10000; ++n) luxstral_signal_buffer_consumed();
    assert(luxstral_wait_for_buffer_consumed() == 1);
    assert(luxstral_wait_for_buffer_consumed() == 0);
    pthread_t worker;
    const clock_t cpu0 = clock();
    luxstral_signal_buffer_consumed();
    pthread_create(&worker, NULL, producer, NULL);
    const struct timespec period = {0, 1000000};
    for (int n = 1; n <= 1000; ++n) {
        while (atomic_load_explicit(&produced, memory_order_acquire) < n) nanosleep(&period, NULL);
        if (n < 1000) luxstral_signal_buffer_consumed();
    }
    pthread_join(worker, NULL);
    printf("PASS: 1000 handoffs, duplicate coalescing, timeout; process CPU %.3fs\n", (double)(clock()-cpu0)/CLOCKS_PER_SEC);
}
