/*
 * synth_luxstral_state.h
 *
 * State management for additive synthesis
 * Contains freeze/fade functionality and display buffer management
 *
 * Author: zhonx
 */

#ifndef __SYNTH_LUXSTRAL_STATE_H__
#define __SYNTH_LUXSTRAL_STATE_H__

/* Includes ------------------------------------------------------------------*/
#include "vst_adapters_c.h"
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported variables --------------------------------------------------------*/

/* Synth Data Freeze Feature
 * NOTE: the freeze/fade flags, buffers and mutexes now live in LuxStralEngine
 * (luxstral_engine.h). External consumers must use the accessor functions
 * declared there. Only the immutable fade duration stays global.             */
extern const double G_SYNTH_DATA_FADE_DURATION_SECONDS;

/* Exported function prototypes ----------------------------------------------*/

/* Synth data freeze/fade management */
void synth_data_freeze_init(void);
void synth_data_freeze_cleanup(void);
double synth_getCurrentTimeInSeconds(void);

/* M8 — init the freeze/fade state (mutex + frozen buffer) for a GIVEN engine.
 * synth_data_freeze_init() above is the engine-A wrapper; engine B calls this. */
struct LuxStralEngine;
void synth_data_freeze_init_engine(struct LuxStralEngine *eng);

/* Display buffer management */
void displayable_synth_buffers_init(void);
void displayable_synth_buffers_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* __SYNTH_LUXSTRAL_STATE_H__ */
