#include "synthesis/luxstral/synth_work_dispatch.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <stdatomic.h>
/* Independent partitions use ordinary payload, not atomics. TSan verifies
 * job publication and completion, including workers that finish out of order. */
typedef struct { SynthWorkDispatch* dispatch; unsigned job, output[16]; } Test;
typedef struct { Test* test; int index; } Worker;
static void work(Test* t, int partition) { t->output[partition] = t->job * 17u + (unsigned)partition; }
static void* run(void* ptr) {
    Worker* w = ptr;
    while (synth_work_dispatch_wait(w->test->dispatch, w->index-1)) {
        work(w->test, w->index);
        synth_work_dispatch_complete(w->test->dispatch);
    }
    return NULL;
}
int main(void) {
    assert(!synth_work_dispatch_create(-1));
    assert(!synth_work_dispatch_create(16));
    for (int n = 1; n <= 16; ++n) for (int cycle = 0; cycle < 3; ++cycle) {
        Test t = {0}; Worker workers[16]; pthread_t threads[16];
        t.dispatch = synth_work_dispatch_create(n-1); assert(t.dispatch);
        for (int i = 1; i < n; ++i) {
            workers[i] = (Worker){&t,i};
            assert(pthread_create(&threads[i],NULL,run,&workers[i]) == 0);
        }
        for (unsigned job = 1; job <= 500; ++job) {
            t.job = job;
            synth_work_dispatch_begin(t.dispatch);
            work(&t,0);
            synth_work_dispatch_finish(t.dispatch);
            for (int i = 0; i < n; ++i) assert(t.output[i] == job*17u+(unsigned)i);
        }
        synth_work_dispatch_stop(t.dispatch);
        synth_work_dispatch_stop(t.dispatch); // idempotent
        for (int i = 1; i < n; ++i) pthread_join(threads[i],NULL);
        synth_work_dispatch_destroy(t.dispatch);
    }
    // Partial thread creation: unstarted auxiliaries must not prevent stopping.
    Test t = {0}; t.dispatch = synth_work_dispatch_create(15);
    Worker w = {&t,1}; pthread_t thread;
    pthread_create(&thread,NULL,run,&w);
    synth_work_dispatch_stop(t.dispatch);
    pthread_join(thread,NULL);
    synth_work_dispatch_destroy(t.dispatch);
    puts("PASS: dispatch publication/completion, 1..16 partitions, restart and partial startup");
}
