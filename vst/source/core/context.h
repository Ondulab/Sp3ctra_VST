#ifndef CONTEXT_H
#define CONTEXT_H

#include "audio_c_api.h"
#include "audio_image_buffers.h"
#include "config.h"
#include "doublebuffer.h"
#include <pthread.h>
#include <time.h>
#include <stdint.h>

// SFML completely removed (core audio only)
// DMX removed - stub type for backward compatibility during cleanup
typedef struct {
  int fd;
  int running;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  void *spots;  // Opaque pointer
  int num_spots;
} DMXContext;

typedef struct {
  void *window;  // Opaque pointer (SFML removed)
  int socket;
  struct sockaddr_in *si_other;
  struct sockaddr_in *si_me;
  AudioData *audioData;
  DoubleBuffer *doubleBuffer;           // Legacy double buffer
  AudioImageBuffers *audioImageBuffers; // Audio buffer system
  DMXContext *dmxCtx;                   // Stub (will be removed later)
  volatile int running;                 // Controls UDP thread (and general shutdown)
  volatile int audio_thread_running;    // 🔧 SEPARATE flag for audio thread (VST buffer size changes)
  volatile int luxsynth_thread_running; // Controls LuxSynth processing thread

  /* IMU state (protected by imu_mutex) */
  pthread_mutex_t imu_mutex;
  float imu_x_filtered;
  float imu_raw_x;
  float imu_raw_y;
  float imu_raw_z;
  float imu_gyro_x;
  float imu_gyro_y;
  float imu_gyro_z;
  float imu_position_x;
  float imu_position_y;
  float imu_position_z;
  float imu_angle_x;
  float imu_angle_y;
  float imu_angle_z;
  time_t last_imu_time;
  int imu_has_value;
} Context;

#endif /* CONTEXT_H */
