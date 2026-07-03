/*
 * rt_profiler.h
 *
 * Real-time performance profiler for audio callback monitoring
 * Measures latency, underruns, and mutex contention
 *
 * Author: zhonx
 * Created: 2025-11-21
 */

#ifndef RT_PROFILER_H
#define RT_PROFILER_H

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Performance thresholds (in microseconds) */
#define RT_PROFILER_WARN_LATENCY_PERCENT    50.0f  /* Warn if callback > 50% of budget */
#define RT_PROFILER_CRITICAL_LATENCY_PERCENT 80.0f  /* Critical if > 80% */
#define RT_PROFILER_WARN_MUTEX_WAIT_US      50     /* Warn if mutex wait > 10µs */
#define RT_PROFILER_CRITICAL_MUTEX_WAIT_US  100    /* Critical if > 100µs */

/* Reporting interval */
#define RT_PROFILER_REPORT_INTERVAL_FRAMES  500   /* Report every 500 frames (~10s @ 48kHz, ~5s @ 96kHz) */

/**
 * @brief Real-time profiler structure
 * 
 * Tracks audio callback performance metrics:
 * - Callback execution time (latency)
 * - Underrun detection
 * - Mutex contention
 */
typedef struct {
    /* Audio callback metrics */
    uint64_t callback_count;
    uint64_t total_callback_time_us;
    uint64_t max_callback_time_us;
    uint64_t callback_budget_us;  /* Maximum allowed time per callback */
    
    /* Underrun tracking (atomic for thread safety) */
    atomic_uint_fast64_t underrun_count;
    
    /* Buffer miss tracking (atomic for thread safety) */
    atomic_uint_fast64_t buffer_miss_luxstral;      /* ready=0, silence output */
    atomic_uint_fast64_t buffer_miss_luxsynth;
    atomic_uint_fast64_t buffer_miss_luxwave;
    /* Stale-buffer re-output: producer is mid-write, consumer re-outputs last frame */
    atomic_uint_fast64_t buffer_stale_luxstral;     /* "SAME DATA" re-output count */
    
    /* Mutex contention tracking */
    uint64_t mutex_lock_attempts;
    uint64_t mutex_contentions;      /* Times trylock failed */
    uint64_t mutex_total_wait_us;
    uint64_t mutex_max_wait_us;
    
    /* Configuration */
    int sample_rate;
    int buffer_size;
    int enabled;  /* 0 = disabled, 1 = enabled */
    
    /* Timing helper */
    struct timeval callback_start_time;
    
    /* Thread performance tracking */
    atomic_uint_fast64_t audio_thread_total_time_us;
    atomic_uint_fast64_t audio_thread_iteration_count;
    atomic_uint_fast64_t audio_thread_max_time_us;
    
    atomic_uint_fast64_t udp_thread_total_time_us;
    atomic_uint_fast64_t udp_thread_packet_count;
    atomic_uint_fast64_t udp_thread_max_time_us;

    /* Deferred logging (RT-safe): RT threads only set flags/counters here,
     * the message thread drains them via rt_profiler_flush_logs().
     * Logging directly from the audio/synthesis threads (logger mutex +
     * localtime + fprintf) blocked the callback and amplified the very
     * latency being reported. */
    atomic_int           report_due;                  /* periodic stats report pending */
    atomic_uint_fast64_t critical_latency_events;     /* callbacks > critical budget since last flush */
    uint64_t             critical_latency_worst_us;   /* worst offender since last flush (approx) */
    atomic_uint_fast64_t underrun_events;             /* underruns since last flush */
    atomic_uint_fast64_t mutex_critical_wait_events;  /* critical mutex waits since last flush */
    atomic_uint_fast64_t mutex_warn_wait_events;      /* long mutex waits since last flush */
    atomic_uint_fast64_t mutex_contention_events;     /* contentions since last flush */
} RTProfiler;

/**
 * @brief Initialize the RT profiler
 * 
 * @param profiler Profiler instance
 * @param sample_rate Audio sample rate (e.g., 48000)
 * @param buffer_size Audio buffer size in frames (e.g., 128)
 */
void rt_profiler_init(RTProfiler *profiler, int sample_rate, int buffer_size);

/**
 * @brief Enable/disable profiling
 * 
 * @param profiler Profiler instance
 * @param enabled 1 to enable, 0 to disable
 */
void rt_profiler_set_enabled(RTProfiler *profiler, int enabled);

/**
 * @brief Mark the start of an audio callback
 * Call this at the beginning of the audio callback
 * 
 * @param profiler Profiler instance
 */
void rt_profiler_callback_start(RTProfiler *profiler);

/**
 * @brief Mark the end of an audio callback
 * Call this at the end of the audio callback
 * Automatically reports stats every N frames
 * 
 * @param profiler Profiler instance
 */
void rt_profiler_callback_end(RTProfiler *profiler);

/**
 * @brief Report an audio underrun
 * Call this when RtAudio reports RTAUDIO_OUTPUT_UNDERFLOW
 * 
 * @param profiler Profiler instance
 */
void rt_profiler_report_underrun(RTProfiler *profiler);

/**
 * @brief Report a buffer miss for additive synthesis
 * Call when the additive synthesis buffer is not ready (ready=0, outputs silence)
 * 
 * @param profiler Profiler instance
 */
void rt_profiler_report_buffer_miss_luxstral(RTProfiler *profiler);

/**
 * @brief Report a stale-buffer re-output for additive synthesis
 * Call when processBlock re-outputs the same buffer because the producer
 * is still mid-write (readIdx == lastConsumedReadIdx → "SAME DATA" branch).
 * Stale outputs are better than silence but reveal producer latency.
 * 
 * @param profiler Profiler instance
 */
void rt_profiler_report_stale_luxstral(RTProfiler *profiler);

/**
 * @brief Report a buffer miss for polyphonic synthesis
 * Call when the polyphonic synthesis buffer is not ready
 * 
 * @param profiler Profiler instance
 */
void rt_profiler_report_buffer_miss_luxsynth(RTProfiler *profiler);

/**
 * @brief Report a buffer miss for photowave synthesis
 * Call when the photowave synthesis buffer is not ready
 * 
 * @param profiler Profiler instance
 */
void rt_profiler_report_buffer_miss_luxwave(RTProfiler *profiler);

/**
 * @brief Record a mutex lock attempt
 * Call before attempting to lock a mutex
 * 
 * @param profiler Profiler instance
 */
void rt_profiler_mutex_lock_start(RTProfiler *profiler);

/**
 * @brief Record a successful mutex lock
 * Call after successfully acquiring a mutex
 * 
 * @param profiler Profiler instance
 * @param wait_time_us Time spent waiting for the lock (in microseconds)
 */
void rt_profiler_mutex_lock_end(RTProfiler *profiler, uint64_t wait_time_us);

/**
 * @brief Record a mutex contention (trylock failed)
 * Call when pthread_mutex_trylock returns EBUSY
 * 
 * @param profiler Profiler instance
 */
void rt_profiler_mutex_contention(RTProfiler *profiler);

/**
 * @brief Print performance statistics
 * Call this periodically from a non-RT thread
 *
 * @param profiler Profiler instance
 */
void rt_profiler_print_stats(RTProfiler *profiler);

/**
 * @brief Flush deferred RT-thread log events
 * Call this periodically from the message thread. Prints the periodic stats
 * report if one is due, plus one coalesced line per event class accumulated
 * since the last flush (critical latency, underruns, mutex waits...), then
 * resets the event counters. Readings may be slightly out of date (diagnostic).
 *
 * @param profiler Profiler instance
 */
void rt_profiler_flush_logs(RTProfiler *profiler);

/**
 * @brief Reset all statistics
 * 
 * @param profiler Profiler instance
 */
void rt_profiler_reset(RTProfiler *profiler);

/**
 * @brief Get current CPU usage percentage
 * 
 * @param profiler Profiler instance
 * @return CPU usage as percentage of available time budget
 */
float rt_profiler_get_cpu_percent(RTProfiler *profiler);

/**
 * @brief Check if performance is within acceptable limits
 * 
 * @param profiler Profiler instance
 * @return 1 if performance is good, 0 if there are issues
 */
int rt_profiler_is_healthy(RTProfiler *profiler);

/**
 * @brief Report audio processing thread iteration time
 * Call this after each synthesis iteration
 * 
 * @param profiler Profiler instance
 * @param elapsed_us Time spent in iteration (microseconds)
 */
void rt_profiler_report_audio_thread_iteration(RTProfiler *profiler, uint64_t elapsed_us);

/**
 * @brief Report UDP thread packet processing time
 * Call this after processing each UDP packet
 * 
 * @param profiler Profiler instance
 * @param elapsed_us Time spent processing packet (microseconds)
 */
void rt_profiler_report_udp_thread_packet(RTProfiler *profiler, uint64_t elapsed_us);

#ifdef __cplusplus
}
#endif

#endif /* RT_PROFILER_H */
