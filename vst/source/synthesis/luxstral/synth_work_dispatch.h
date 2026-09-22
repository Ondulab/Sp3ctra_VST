#ifndef SYNTH_WORK_DISPATCH_H
#define SYNTH_WORK_DISPATCH_H
#ifdef __cplusplus
extern "C" {
#endif
/* One producer, fixed auxiliary workers, one batch in flight. Creation,
 * stopping and destruction require the producer to be outside a batch.
 * The producer computes partition zero itself while auxiliaries run. */
typedef struct SynthWorkDispatch SynthWorkDispatch;
SynthWorkDispatch* synth_work_dispatch_create(int auxiliaries);
void synth_work_dispatch_begin(SynthWorkDispatch*);
void synth_work_dispatch_finish(SynthWorkDispatch*);
int synth_work_dispatch_wait(SynthWorkDispatch*, int auxiliary);
void synth_work_dispatch_complete(SynthWorkDispatch*);
void synth_work_dispatch_stop(SynthWorkDispatch*);
/* Destroy only after all auxiliary threads have been joined. */
void synth_work_dispatch_destroy(SynthWorkDispatch*);
#ifdef __cplusplus
}
#endif
#endif
