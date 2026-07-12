/* audio_c_api.h - C interface for RtAudio */

#ifndef audio_h
#define audio_h

#include "config.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

// Types compatibles avec CoreAudio
typedef float Float32;
typedef uint32_t UInt32;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AudioData {
  Float32 **buffers;
  UInt32 numChannels;
  UInt32 bufferSize;
} AudioData;

typedef struct {
  float *data; // dynamically allocated with size = g_sp3ctra_config.audio_buffer_size
  int ready; // 0: libre, 1: rempli et en attente de lecture
  uint64_t write_timestamp_us; // Timestamp when buffer was written (microseconds)
  pthread_mutex_t mutex;
  pthread_cond_t cond;
} AudioDataBuffers;

extern AudioDataBuffers buffers_L[2];
extern AudioDataBuffers buffers_R[2];

extern volatile int current_buffer_index;
extern pthread_mutex_t buffer_index_mutex;

// (Purge 2026-07-12: the legacy RtAudio C API declarations — audio_Init/
// startAudioUnit/device & mix/reverb-level setters — had NO definitions in
// the plugin; JUCE owns the audio device. Only the shared buffer types and
// externs above are alive.)

#ifdef __cplusplus
}
#endif

#endif /* audio_h */
