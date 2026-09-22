/*
 * synth_luxstral_threading_rt.c
 *
 * Real-time deterministic threading extensions
 * Implements barrier synchronization and RT priorities
 *
 * Author: zhonx
 */

#include "vst_adapters_c.h"
#include "synth_luxstral_threading.h"
#include "luxstral_engine.h"
#include <errno.h>
#include <string.h>

#ifdef __linux__
#include <sched.h>
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <mach/thread_act.h>
#include <pthread/qos.h>
#endif

/* macOS barrier implementation (pthread_barrier not available) */
#ifndef __linux__

// Define PTHREAD_BARRIER_SERIAL_THREAD for macOS compatibility
#ifndef PTHREAD_BARRIER_SERIAL_THREAD
#define PTHREAD_BARRIER_SERIAL_THREAD -1
#endif

int barrier_init(barrier_t *barrier, int count) {
  if (count <= 0) {
    return EINVAL;
  }
  
  barrier->count = count;
  barrier->waiting = 0;
  barrier->generation = 0;
  
  if (pthread_mutex_init(&barrier->mutex, NULL) != 0) {
    return errno;
  }
  
  if (pthread_cond_init(&barrier->cond, NULL) != 0) {
    pthread_mutex_destroy(&barrier->mutex);
    return errno;
  }
  
  return 0;
}

int barrier_wait(LuxStralEngine *eng, barrier_t *barrier) {
  // 🔧 CRITICAL FIX: Check exit flag before waiting (per-engine flags)
  if (eng->workers_must_exit || eng->pool_shutdown) {
    return -1;  // Early exit - thread should terminate
  }

  pthread_mutex_lock(&barrier->mutex);

  // Check again under lock
  if (eng->workers_must_exit || eng->pool_shutdown) {
    pthread_mutex_unlock(&barrier->mutex);
    return -1;
  }

  int gen = barrier->generation;
  barrier->waiting++;

  if (barrier->waiting >= barrier->count) {
    // Last thread to arrive - wake everyone up
    barrier->waiting = 0;
    barrier->generation++;
    pthread_cond_broadcast(&barrier->cond);
    pthread_mutex_unlock(&barrier->mutex);
    return PTHREAD_BARRIER_SERIAL_THREAD;  // Special return for last thread
  }

  // Wait for all threads to arrive, but check exit flags on each wakeup
  while (gen == barrier->generation) {
    pthread_cond_wait(&barrier->cond, &barrier->mutex);

    // 🔧 FIX: Check exit flags after wakeup from broadcast
    // If generation has already advanced (last thread reset waiting=0),
    // do NOT decrement waiting — it's already 0 and would underflow to -1,
    // corrupting the barrier for any subsequent reuse.
    if (eng->workers_must_exit || eng->pool_shutdown) {
      if (gen == barrier->generation) {
        // Generation hasn't advanced yet: we're still in the wait set
        barrier->waiting--;
      }
      // If gen != barrier->generation, the last thread already reset waiting=0
      pthread_mutex_unlock(&barrier->mutex);
      return -1;  // Early exit
    }
  }

  pthread_mutex_unlock(&barrier->mutex);
  return 0;
}

int barrier_destroy(barrier_t *barrier) {
  pthread_mutex_destroy(&barrier->mutex);
  pthread_cond_destroy(&barrier->cond);
  return 0;
}

#endif /* !__linux__ */

/**
 * @brief  Initialize barrier synchronization system
 * @param  eng Engine instance
 * @param  num_threads Number of threads (workers + main thread)
 * @retval 0 on success, -1 on error
 */
int synth_init_barriers(LuxStralEngine *eng, int num_threads) {
#ifdef __linux__
  if (pthread_barrier_init(&eng->worker_start_barrier, NULL, num_threads) != 0) {
    log_error("SYNTH_RT", "Failed to initialize start barrier");
    return -1;
  }
  if (pthread_barrier_init(&eng->worker_end_barrier, NULL, num_threads) != 0) {
    log_error("SYNTH_RT", "Failed to initialize end barrier");
    pthread_barrier_destroy(&eng->worker_start_barrier);
    return -1;
  }
#else
  if (barrier_init(&eng->worker_start_barrier, num_threads) != 0) {
    log_error("SYNTH_RT", "Failed to initialize start barrier");
    return -1;
  }
  if (barrier_init(&eng->worker_end_barrier, num_threads) != 0) {
    log_error("SYNTH_RT", "Failed to initialize end barrier");
    barrier_destroy(&eng->worker_start_barrier);
    return -1;
  }
#endif

  log_startup_detail("SYNTH_RT", "Barrier synchronization initialized for %d threads", num_threads);
  return 0;
}

/**
 * @brief  Cleanup barrier synchronization system
 * @param  eng Engine instance
 * @retval None
 */
void synth_cleanup_barriers(LuxStralEngine *eng) {
#ifdef __linux__
  pthread_barrier_destroy(&eng->worker_start_barrier);
  pthread_barrier_destroy(&eng->worker_end_barrier);
#else
  barrier_destroy(&eng->worker_start_barrier);
  barrier_destroy(&eng->worker_end_barrier);
#endif
  log_info("SYNTH_RT", "Barrier synchronization cleaned up");
}

/**
 * @brief  Set real-time priority for a thread
 * @param  thread Thread handle
 * @param  priority Priority level (1-99, higher = more priority)
 * @retval 0 on success, -1 on error
 */
static int synth_set_rt_priority_for_format(pthread_t thread, int priority,
                                            int requested_rate, int requested_size) {
#ifdef __linux__
  struct sched_param param;
  param.sched_priority = priority;
  
  int result = pthread_setschedparam(thread, SCHED_FIFO, &param);
  if (result != 0) {
    log_warning("SYNTH_RT", "Failed to set RT priority %d: %s (error %d)", 
                priority, strerror(result), result);
    log_warning("SYNTH_RT", "Note: RT priorities require CAP_SYS_NICE capability or rtprio limits");
    return -1;
  }
  
  log_info("SYNTH_RT", "Set RT priority %d (SCHED_FIFO)", priority);
  return 0;
  
#elif defined(__APPLE__)
  // macOS: Multi-layer RT priority boost for maximum scheduling priority
  // This ensures synthesis workers run before all external processes
  (void)priority; // Unused on macOS - uses Mach policies instead
  
  
  // Get the Mach thread from pthread
  mach_port_t mach_thread = pthread_mach_thread_np(thread);
  
  // ========================================================================
  // LAYER 1: QoS Class (User Interactive = highest non-RT QoS)
  // Note: QoS is set per-thread inside synth_persistent_worker_thread() via
  // pthread_set_qos_class_self_np() - we cannot set it from here
  // ========================================================================
  // QoS setup is handled inside the worker thread function itself
  log_startup_detail("SYNTH_RT", "QoS will be set by worker thread on startup");
  
  // ========================================================================
  // LAYER 2: Thread Precedence Policy (additional priority boost)
  // Increases relative importance within same RT scheduling band
  // ========================================================================
  thread_precedence_policy_data_t precedence;
  precedence.importance = 63;  // Maximum precedence (0-63)
  
  kern_return_t prec_result = thread_policy_set(
      mach_thread,
      THREAD_PRECEDENCE_POLICY,
      (thread_policy_t)&precedence,
      THREAD_PRECEDENCE_POLICY_COUNT
  );
  if (prec_result == KERN_SUCCESS) {
    log_startup_detail("SYNTH_RT", "Precedence policy set to maximum (63)");
  }
  
  // ========================================================================
  // LAYER 3: Time-Constraint Policy (hard RT scheduling)
  // Tells the kernel this thread has strict real-time deadlines
  // ========================================================================
  // Calculate time constraints dynamically based on actual buffer size
  // Use g_sp3ctra_config if available, otherwise use safe defaults
  const int sample_rate = requested_rate > 0 ? requested_rate : 48000;
  const int buffer_size = requested_size > 0 ? requested_size : 128;
  
  // Calculate period in nanoseconds: (buffer_size / sample_rate) * 1e9
  uint64_t audio_period_ns = (uint64_t)buffer_size * 1000000000ULL / (uint64_t)sample_rate;
  
  // Convert nanoseconds to Mach absolute time units
  mach_timebase_info_data_t timebase;
  mach_timebase_info(&timebase);
  
  // Convert to Mach time units (depends on CPU frequency)
  uint32_t period_mach = (uint32_t)((audio_period_ns * timebase.denom) / timebase.numer);
  
  // 🔧 AGGRESSIVE RT SETTINGS for synthesis workers:
  // - computation: 75% of period (observed dense rendering is >50%)
  // - constraint: 95% of period (tight deadline for RT behavior)
  // - preemptible: TRUE (modern XNU ignores this legacy field)
  thread_time_constraint_policy_data_t policy;
  policy.period      = period_mach;
  policy.computation = (uint32_t)(period_mach * 0.75);  // reserve 75% of the real period
  policy.constraint  = (uint32_t)(period_mach * 0.95);  // 95% hard deadline
  policy.preemptible = TRUE;   // permit preemption on kernels that honor this field
  
  kern_return_t result = thread_policy_set(
      mach_thread,
      THREAD_TIME_CONSTRAINT_POLICY,
      (thread_policy_t)&policy,
      THREAD_TIME_CONSTRAINT_POLICY_COUNT
  );
  
  if (result == KERN_SUCCESS) {
    log_startup_detail("SYNTH_RT", "Time-constraint policy: period=%.2fms, computation=%.2fms, constraint=%.2fms, preemptible=YES",
             audio_period_ns / 1000000.0,
             (audio_period_ns * 0.75) / 1000000.0,
             (audio_period_ns * 0.95) / 1000000.0);
  } else {
    log_warning("SYNTH_RT", "Failed to set time-constraint policy (error %d)", result);
    log_info("SYNTH_RT", "Note: Full RT requires elevated privileges on macOS");
  }
  
  // Precedence alone is NOT real-time scheduling. The caller needs to know
  // whether the time-constraint policy actually succeeded.
  if (result == KERN_SUCCESS) {
    return 0;
  }

  log_warning("SYNTH_RT", "All RT policy attempts failed - continuing without RT");
  return -1;

#elif defined(_WIN32)
  // Windows: ABOVE_NORMAL for the workers, NOT TIME_CRITICAL. Field traces
  // (12-core desktop + laptop, 2026-07-28) showed the whole pool at
  // TIME_CRITICAL starving the WASAPI feeder and the system: load ratcheted
  // to 150%+ of budget with stale re-outs. The MMCSS "Pro Audio" class the
  // worker joins on startup already grants elevated scheduling; only the
  // single audio pacer thread (AudioProcessingThread) keeps TIME_CRITICAL.
  (void)priority;
  if (SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL)) {
    log_startup_detail("SYNTH_RT", "Worker priority set to ABOVE_NORMAL (+MMCSS)");
    return 0;
  }
  log_warning("SYNTH_RT", "SetThreadPriority(ABOVE_NORMAL) failed (error %lu)",
              (unsigned long)GetLastError());
  return -1;

#else
  log_warning("SYNTH_RT", "RT priorities not supported on this platform");
  return -1;
#endif
}

/**
 * @brief  Wrapper for barrier wait (cross-platform)
 * @param  eng Engine instance (exit flags checked on macOS path)
 * @param  barrier Barrier to wait on
 * @retval 0 on success, PTHREAD_BARRIER_SERIAL_THREAD for last thread
 */
int synth_barrier_wait(LuxStralEngine *eng, void *barrier) {
#ifdef __linux__
  (void)eng;
  return pthread_barrier_wait((pthread_barrier_t*)barrier);
#else
  return barrier_wait(eng, (barrier_t*)barrier);
#endif
}

int synth_set_rt_priority(pthread_t thread, int priority) {
  return synth_set_rt_priority_for_format(thread, priority,
      g_sp3ctra_config.sampling_frequency, g_sp3ctra_config.audio_buffer_size);
}

#ifdef __APPLE__
static void synth_restore_timesharing(pthread_t thread) {
  mach_port_t port = pthread_mach_thread_np(thread);
  thread_extended_policy_data_t extended = { .timeshare = TRUE };
  thread_precedence_policy_data_t precedence = { .importance = 0 };
  thread_policy_set(port, THREAD_EXTENDED_POLICY, (thread_policy_t)&extended,
                    THREAD_EXTENDED_POLICY_COUNT);
  thread_policy_set(port, THREAD_PRECEDENCE_POLICY, (thread_policy_t)&precedence,
                    THREAD_PRECEDENCE_POLICY_COUNT);
}
#endif

void synth_update_realtime_team_policy(LuxStralEngine *eng) {
#ifdef __APPLE__
  static _Thread_local const LuxStralEngine *configured_engine;
  const int sr = g_sp3ctra_config.sampling_frequency;
  const int bs = g_sp3ctra_config.audio_buffer_size;
  // A replacement producer needs its own policy even when the format is unchanged.
  if (configured_engine == eng && eng->scheduling_sample_rate == sr &&
      eng->scheduling_block_size == bs)
    return;
  // Called on the producer while every auxiliary is parked, before begin().
  // Equal priority lets partition zero overlap the auxiliaries. With a QoS
  // producer and RT auxiliaries, waking them can suspend the producer until
  // their computation finishes, serializing two otherwise parallel phases.
  int ok = synth_set_rt_priority_for_format(pthread_self(), 80, sr, bs) == 0;
  for (int i = 1; i <= eng->started_auxiliaries; ++i)
    if (synth_set_rt_priority_for_format(eng->worker_threads[i], 80, sr, bs) != 0) ok = 0;
  if (!ok) {
    // A partial success would recreate the priority mismatch. Keep the team
    // in the same ordinary scheduling class if RT cannot be established.
    synth_restore_timesharing(pthread_self());
    for (int i = 1; i <= eng->started_auxiliaries; ++i)
      synth_restore_timesharing(eng->worker_threads[i]);
  }
  configured_engine = eng;
  eng->scheduling_sample_rate = sr;
  eng->scheduling_block_size = bs;
  log_info("SYNTH_SCHED", "producer + %d auxiliaries: %s, %d Hz / %d samples, period %.3f ms",
           eng->started_auxiliaries, ok ? "matched RT policy" : "timesharing fallback",
           sr, bs, sr > 0 ? bs * 1000.0 / sr : 0.0);
#else
  (void)eng;
#endif
}
