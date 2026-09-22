#ifndef SP3CTRA_SPSC_SNAPSHOT_H
#define SP3CTRA_SPSC_SNAPSHOT_H
#include <stdatomic.h>
/* Three payload slots supplied by the caller. Exactly one publisher and one
 * reader. Only the middle slot changes hands; front and back are exclusive.
 * Initialization requires quiescent threads. No retries, allocation or locks. */
typedef struct { int front, back; atomic_int middle; } Sp3ctraSpscSnapshot;
/* The helpers are C-only: C++ TUs just embed the struct via the engine
 * headers, and the Windows <stdatomic.h> mirror gives C++ the types only. */
#ifndef __cplusplus
static inline void sp3ctra_snapshot_init(Sp3ctraSpscSnapshot* s) {
    s->front = 0; s->back = 2; atomic_store(&s->middle, 1);
}
static inline void sp3ctra_snapshot_publish(Sp3ctraSpscSnapshot* s) {
    s->back = atomic_exchange_explicit(&s->middle, s->back | 4, memory_order_acq_rel) & 3;
}
static inline int sp3ctra_snapshot_acquire(Sp3ctraSpscSnapshot* s) {
    if (!(atomic_load_explicit(&s->middle, memory_order_acquire) & 4)) return 0;
    s->front = atomic_exchange_explicit(&s->middle, s->front, memory_order_acq_rel) & 3;
    return 1;
}
#endif /* !__cplusplus */
#endif
