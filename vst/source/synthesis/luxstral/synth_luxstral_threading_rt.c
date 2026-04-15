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

int barrier_wait(barrier_t *barrier) {
  // 🔧 CRITICAL FIX: Check exit flag before waiting
  extern _Atomic int synth_workers_must_exit;
  extern _Atomic int synth_pool_shutdown;
  
  if (synth_workers_must_exit || synth_pool_shutdown) {
    return -1;  // Early exit - thread should terminate
  }
  
  pthread_mutex_lock(&barrier->mutex);
  
  // Check again under lock
  if (synth_workers_must_exit || synth_pool_shutdown) {
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
    if (synth_workers_must_exit || synth_pool_shutdown) {
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
 * @param  num_threads Number of threads (workers + main thread)
 * @retval 0 on success, -1 on error
 */
int synth_init_barriers(int num_threads) {
#ifdef __linux__
  if (pthread_barrier_init(&g_worker_start_barrier, NULL, num_threads) != 0) {
    log_error("SYNTH_RT", "Failed to initialize start barrier");
    return -1;
  }
  if (pthread_barrier_init(&g_worker_end_barrier, NULL, num_threads) != 0) {
    log_error("SYNTH_RT", "Failed to initialize end barrier");
    pthread_barrier_destroy(&g_worker_start_barrier);
    return -1;
  }
#else
  if (barrier_init(&g_worker_start_barrier, num_threads) != 0) {
    log_error("SYNTH_RT", "Failed to initialize start barrier");
    return -1;
  }
  if (barrier_init(&g_worker_end_barrier, num_threads) != 0) {
    log_error("SYNTH_RT", "Failed to initialize end barrier");
    barrier_destroy(&g_worker_start_barrier);
    return -1;
  }
#endif
  
  log_info("SYNTH_RT", "Barrier synchronization initialized for %d threads", num_threads);
  return 0;
}

/**
 * @brief  Cleanup barrier synchronization system
 * @retval None
 */
void synth_cleanup_barriers(void) {
#ifdef __linux__
  pthread_barrier_destroy(&g_worker_start_barrier);
  pthread_barrier_destroy(&g_worker_end_barrier);
#else
  barrier_destroy(&g_worker_start_barrier);
  barrier_destroy(&g_worker_end_barrier);
#endif
  log_info("SYNTH_RT", "Barrier synchronization cleaned up");
}

/**
 * @brief  Set real-time priority for a thread
 * @param  thread Thread handle
 * @param  priority Priority level (1-99, higher = more priority)
 * @retval 0 on success, -1 on error
 */
int synth_set_rt_priority(pthread_t thread, int priority) {
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
  
  int success_count = 0;
  
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
    success_count++;
    log_startup_detail("SYNTH_RT", "Precedence policy set to maximum (63)");
  }
  
  // ========================================================================
  // LAYER 3: Time-Constraint Policy (hard RT scheduling)
  // Tells the kernel this thread has strict real-time deadlines
  // ========================================================================
  // Calculate time constraints dynamically based on actual buffer size
  // Use g_sp3ctra_config if available, otherwise use safe defaults
  extern sp3ctra_config_t g_sp3ctra_config;
  int sample_rate = g_sp3ctra_config.sampling_frequency > 0 ? 
                    g_sp3ctra_config.sampling_frequency : 48000;
  int buffer_size = g_sp3ctra_config.audio_buffer_size > 0 ? 
                    g_sp3ctra_config.audio_buffer_size : 128;
  
  // Calculate period in nanoseconds: (buffer_size / sample_rate) * 1e9
  uint64_t audio_period_ns = (uint64_t)buffer_size * 1000000000ULL / (uint64_t)sample_rate;
  
  // Convert nanoseconds to Mach absolute time units
  mach_timebase_info_data_t timebase;
  mach_timebase_info(&timebase);
  
  // Convert to Mach time units (depends on CPU frequency)
  uint32_t period_mach = (uint32_t)((audio_period_ns * timebase.denom) / timebase.numer);
  
  // 🔧 AGGRESSIVE RT SETTINGS for synthesis workers:
  // - computation: 50% of period (conservative to leave margin)
  // - constraint: 95% of period (tight deadline for RT behavior)
  // - preemptible: FALSE (do not interrupt once running!)
  thread_time_constraint_policy_data_t policy;
  policy.period      = period_mach;
  policy.computation = (uint32_t)(period_mach * 0.5);   // 50% max computation
  policy.constraint  = (uint32_t)(period_mach * 0.95);  // 95% hard deadline
  policy.preemptible = FALSE;  // 🔧 Do NOT preempt synthesis workers!
  
  kern_return_t result = thread_policy_set(
      mach_thread,
      THREAD_TIME_CONSTRAINT_POLICY,
      (thread_policy_t)&policy,
      THREAD_TIME_CONSTRAINT_POLICY_COUNT
  );
  
  if (result == KERN_SUCCESS) {
    success_count++;
    log_startup_detail("SYNTH_RT", "Time-constraint policy: period=%.2fms, computation=%.2fms, constraint=%.2fms, preemptible=NO",
             audio_period_ns / 1000000.0,
             (audio_period_ns * 0.5) / 1000000.0,
             (audio_period_ns * 0.95) / 1000000.0);
  } else {
    log_warning("SYNTH_RT", "Failed to set time-constraint policy (error %d)", result);
    log_info("SYNTH_RT", "Note: Full RT requires elevated privileges on macOS");
  }
  
  // Return success if at least QoS was set (basic priority boost)
  if (success_count >= 1) {
    return 0;
  }
  
  log_warning("SYNTH_RT", "All RT policy attempts failed - continuing without RT");
  return -1;
  
#else
  log_warning("SYNTH_RT", "RT priorities not supported on this platform");
  return -1;
#endif
}

/**
 * @brief  Wrapper for barrier wait (cross-platform)
 * @param  barrier Barrier to wait on
 * @retval 0 on success, PTHREAD_BARRIER_SERIAL_THREAD for last thread
 */
int synth_barrier_wait(void *barrier) {
#ifdef __linux__
  return pthread_barrier_wait((pthread_barrier_t*)barrier);
#else
  return barrier_wait((barrier_t*)barrier);
#endif
}
