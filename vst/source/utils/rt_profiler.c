/*
 * rt_profiler.c
 *
 * Real-time performance profiler implementation
 *
 * Author: zhonx
 * Created: 2025-11-21
 */

#include "rt_profiler.h"
#include "logger.h"
#include <string.h>
#include <stdio.h>

/* Helper function to calculate time difference in microseconds */
static inline uint64_t timeval_diff_us(struct timeval *start, struct timeval *end) {
    return (end->tv_sec - start->tv_sec) * 1000000ULL + 
           (end->tv_usec - start->tv_usec);
}

void rt_profiler_init(RTProfiler *profiler, int sample_rate, int buffer_size) {
    memset(profiler, 0, sizeof(RTProfiler));
    
    profiler->sample_rate = sample_rate;
    profiler->buffer_size = buffer_size;
    profiler->enabled = 1;  /* Enabled by default */
    
    /* Calculate callback budget: (buffer_size / sample_rate) * 1,000,000 µs */
    profiler->callback_budget_us = ((uint64_t)buffer_size * 1000000ULL) / sample_rate;
    
    log_info("RT_PROFILER", "Initialized: %d Hz, %d frames, budget=%llu µs",
             sample_rate, buffer_size, profiler->callback_budget_us);
}

void rt_profiler_set_enabled(RTProfiler *profiler, int enabled) {
    profiler->enabled = enabled;
    if (enabled) {
        log_info("RT_PROFILER", "Profiling enabled");
    } else {
        log_info("RT_PROFILER", "Profiling disabled");
    }
}

void rt_profiler_callback_start(RTProfiler *profiler) {
    if (!profiler->enabled) return;
    
    gettimeofday(&profiler->callback_start_time, NULL);
}

void rt_profiler_callback_end(RTProfiler *profiler) {
    if (!profiler->enabled) return;
    
    struct timeval end_time;
    gettimeofday(&end_time, NULL);
    
    uint64_t elapsed_us = timeval_diff_us(&profiler->callback_start_time, &end_time);
    
    profiler->callback_count++;
    profiler->total_callback_time_us += elapsed_us;
    
    if (elapsed_us > profiler->max_callback_time_us) {
        profiler->max_callback_time_us = elapsed_us;
    }
    
    /* Report stats periodically — DEFERRED: logging from the audio thread
     * (logger mutex + localtime + fprintf) blocked the callback, so we only
     * raise a flag here and rt_profiler_flush_logs() prints on the message
     * thread. */
    if (profiler->callback_count % RT_PROFILER_REPORT_INTERVAL_FRAMES == 0) {
        atomic_store(&profiler->report_due, 1);
    }

    /* Track critical latency — coalesced into ONE line at the next flush
     * (logging every offending callback amplified the very latency reported) */
    float percent = (elapsed_us * 100.0f) / profiler->callback_budget_us;
    if (percent > RT_PROFILER_CRITICAL_LATENCY_PERCENT) {
        atomic_fetch_add(&profiler->critical_latency_events, 1);
        if (elapsed_us > profiler->critical_latency_worst_us) {
            profiler->critical_latency_worst_us = elapsed_us;
        }
    }
}

void rt_profiler_report_underrun(RTProfiler *profiler) {
    if (!profiler->enabled) return;
    
    atomic_fetch_add(&profiler->underrun_count, 1);

    /* Called from the RT callback path — no logging here, events are
     * coalesced by rt_profiler_flush_logs() on the message thread */
    atomic_fetch_add(&profiler->underrun_events, 1);
}

void rt_profiler_report_buffer_miss_luxstral(RTProfiler *profiler) {
    atomic_fetch_add(&profiler->buffer_miss_luxstral, 1);
}

void rt_profiler_report_stale_luxstral(RTProfiler *profiler) {
    atomic_fetch_add(&profiler->buffer_stale_luxstral, 1);
}

void rt_profiler_report_buffer_miss_luxsynth(RTProfiler *profiler) {
    if (!profiler->enabled) return;
    atomic_fetch_add(&profiler->buffer_miss_luxsynth, 1);
}

void rt_profiler_report_buffer_miss_luxwave(RTProfiler *profiler) {
    if (!profiler->enabled) return;
    atomic_fetch_add(&profiler->buffer_miss_luxwave, 1);
}

void rt_profiler_mutex_lock_start(RTProfiler *profiler) {
    if (!profiler->enabled) return;
    
    profiler->mutex_lock_attempts++;
}

void rt_profiler_mutex_lock_end(RTProfiler *profiler, uint64_t wait_time_us) {
    if (!profiler->enabled) return;
    
    profiler->mutex_total_wait_us += wait_time_us;
    
    if (wait_time_us > profiler->mutex_max_wait_us) {
        profiler->mutex_max_wait_us = wait_time_us;
    }
    
    /* Count long waits — called from the synthesis thread, so no logging
     * here; events are coalesced by rt_profiler_flush_logs() */
    if (wait_time_us > RT_PROFILER_CRITICAL_MUTEX_WAIT_US) {
        atomic_fetch_add(&profiler->mutex_critical_wait_events, 1);
    } else if (wait_time_us > RT_PROFILER_WARN_MUTEX_WAIT_US) {
        atomic_fetch_add(&profiler->mutex_warn_wait_events, 1);
    }
}

void rt_profiler_mutex_contention(RTProfiler *profiler) {
    if (!profiler->enabled) return;
    
    profiler->mutex_contentions++;

    /* Called from RT paths — contentions are logged coalesced by
     * rt_profiler_flush_logs() on the message thread */
    atomic_fetch_add(&profiler->mutex_contention_events, 1);
}

void rt_profiler_print_stats(RTProfiler *profiler) {
    if (!profiler->enabled || profiler->callback_count == 0) return;

    uint64_t avg_callback_us = profiler->total_callback_time_us / profiler->callback_count;

    uint64_t underruns   = atomic_load(&profiler->underrun_count);
    uint64_t miss        = atomic_load(&profiler->buffer_miss_luxstral);
    uint64_t stale       = atomic_load(&profiler->buffer_stale_luxstral);
    float    miss_rate   = (profiler->callback_count > 0)
                               ? (miss  * 100.0f) / profiler->callback_count : 0.0f;
    float    stale_rate  = (profiler->callback_count > 0)
                               ? (stale * 100.0f) / profiler->callback_count : 0.0f;

    /* AudioProcessingThread (synthesis) snapshot */
    uint64_t audio_iterations = atomic_load(&profiler->audio_thread_iteration_count);
    uint64_t audio_avg = 0;
    uint64_t audio_max = atomic_load(&profiler->audio_thread_max_time_us);
    if (audio_iterations > 0) {
        uint64_t audio_total = atomic_load(&profiler->audio_thread_total_time_us);
        audio_avg = audio_total / audio_iterations;
    }
    float audio_ratio     = (profiler->callback_budget_us > 0)
                                ? (audio_avg * 100.0f) / profiler->callback_budget_us : 0.0f;
    float audio_max_ratio = (profiler->callback_budget_us > 0)
                                ? (audio_max * 100.0f) / profiler->callback_budget_us : 0.0f;

    float callback_ratio  = (profiler->callback_budget_us > 0)
                                ? (avg_callback_us * 100.0f) / profiler->callback_budget_us : 0.0f;

    /*
     * ONE-LINE SUMMARY always visible at LOG_LEVEL_INFO.
     * Key fields:
     *   budget   = time allowed per callback (µs)
     *   synth    = average synth iteration time (µs) / % of budget
     *   miss     = fraction of callbacks where buffer was not ready (silence)
     *   stale    = fraction of callbacks that re-output the previous buffer
     *              because the producer had not yet finished writing
     */
    log_info("RT_PROFILER",
        "[%d Hz / %d frm | budget %llu µs] "
        "synth avg %llu µs (%.0f%%) max %llu µs (%.0f%%) | "
        "cb %.0f%% | miss %.2f%% | stale(re-out) %.2f%% | underruns %llu",
        profiler->sample_rate, profiler->buffer_size,
        profiler->callback_budget_us,
        audio_avg,  audio_ratio,
        audio_max,  audio_max_ratio,
        callback_ratio,
        miss_rate,
        stale_rate,
        underruns);

    /* Warn immediately on any stale re-output (producer is slower than consumer) */
    if (stale > 0 && stale_rate > 1.0f) {
        log_warning("RT_PROFILER",
            "Producer is SLOWER than consumer: %.1f%% of callbacks re-output stale buffer "
            "(synth %.0f µs avg vs budget %llu µs). "
            "Risk of phase discontinuities / crackling at high SR.",
            stale_rate, (float)audio_avg,
            profiler->callback_budget_us);
    }

    /* ---- detailed breakdown (log_debug only) ---- */
    log_debug("RT_PROFILER", "  processBlock: avg %llu µs (%.1f%%), max %llu µs, calls %llu",
              avg_callback_us, callback_ratio,
              profiler->max_callback_time_us, profiler->callback_count);

    if (audio_iterations > 0) {
        log_debug("RT_PROFILER",
                  "  synth thread: avg %llu µs (%.1f%%), max %llu µs (%.1f%%), iters %llu",
                  audio_avg, audio_ratio, audio_max, audio_max_ratio, audio_iterations);
    }

    log_debug("RT_PROFILER",
              "  buffer: miss %llu (%.2f%%), stale %llu (%.2f%%), underruns %llu",
              miss, miss_rate, stale, stale_rate, underruns);

    if (!rt_profiler_is_healthy(profiler)) {
        log_warning("RT_PROFILER", "PERFORMANCE ISSUES DETECTED");
    }

    /* Reset per-period synthesis counters */
    if (audio_iterations > 0) {
        atomic_store(&profiler->audio_thread_total_time_us,    0);
        atomic_store(&profiler->audio_thread_iteration_count,  0);
        atomic_store(&profiler->audio_thread_max_time_us,      0);
    }

    /* Reset per-period UDP counters */
    uint64_t udp_packets = atomic_load(&profiler->udp_thread_packet_count);
    if (udp_packets > 0) {
        atomic_store(&profiler->udp_thread_total_time_us,  0);
        atomic_store(&profiler->udp_thread_packet_count,   0);
        atomic_store(&profiler->udp_thread_max_time_us,    0);
    }
}

void rt_profiler_flush_logs(RTProfiler *profiler) {
    /* Message-thread only. RT threads never log — they raise the flags and
     * counters drained here. Slightly stale readings are fine (diagnostic). */

    /* Periodic stats report requested by the audio callback */
    if (atomic_exchange(&profiler->report_due, 0)) {
        rt_profiler_print_stats(profiler);
    }

    /* Coalesced critical-latency events (one line per flush, not per callback) */
    uint64_t crit_latency = atomic_exchange(&profiler->critical_latency_events, 0);
    if (crit_latency > 0) {
        uint64_t worst_us = profiler->critical_latency_worst_us;
        profiler->critical_latency_worst_us = 0;
        log_warning("RT_PROFILER",
                    "CRITICAL latency: %llu callback(s) > %.0f%% of budget since last flush (worst %llu µs)",
                    crit_latency, RT_PROFILER_CRITICAL_LATENCY_PERCENT, worst_us);
    }

    /* Coalesced underruns (they should be rare) */
    uint64_t underruns = atomic_exchange(&profiler->underrun_events, 0);
    if (underruns > 0) {
        log_error("RT_PROFILER", "UNDERRUN x%llu since last flush (total %llu)",
                  underruns, (uint64_t)atomic_load(&profiler->underrun_count));
    }

    /* Coalesced mutex-wait warnings */
    uint64_t mutex_crit = atomic_exchange(&profiler->mutex_critical_wait_events, 0);
    if (mutex_crit > 0) {
        log_warning("RT_PROFILER", "CRITICAL mutex wait x%llu since last flush (max %llu µs)",
                    mutex_crit, profiler->mutex_max_wait_us);
    }
    uint64_t mutex_warn = atomic_exchange(&profiler->mutex_warn_wait_events, 0);
    if (mutex_warn > 0) {
        log_warning("RT_PROFILER", "Long mutex wait x%llu since last flush",
                    mutex_warn);
    }

    /* Coalesced mutex contentions (should be rare in well-designed RT code) */
    uint64_t contentions = atomic_exchange(&profiler->mutex_contention_events, 0);
    if (contentions > 0) {
        log_warning("RT_PROFILER", "Mutex contention x%llu since last flush (total %llu)",
                    contentions, profiler->mutex_contentions);
    }
}

void rt_profiler_reset(RTProfiler *profiler) {
    uint64_t underruns = atomic_load(&profiler->underrun_count);
    
    profiler->callback_count = 0;
    profiler->total_callback_time_us = 0;
    profiler->max_callback_time_us = 0;
    atomic_store(&profiler->underrun_count, 0);
    atomic_store(&profiler->buffer_miss_luxstral, 0);
    atomic_store(&profiler->buffer_miss_luxsynth, 0);
    atomic_store(&profiler->buffer_miss_luxwave, 0);
    atomic_store(&profiler->buffer_stale_luxstral, 0);
    profiler->mutex_lock_attempts = 0;
    profiler->mutex_contentions = 0;
    profiler->mutex_total_wait_us = 0;
    profiler->mutex_max_wait_us = 0;
    
    log_info("RT_PROFILER", "Stats reset (had %llu underruns)", underruns);
}

float rt_profiler_get_cpu_percent(RTProfiler *profiler) {
    if (profiler->callback_count == 0 || profiler->callback_budget_us == 0) {
        return 0.0f;
    }
    
    uint64_t avg_callback_us = profiler->total_callback_time_us / profiler->callback_count;
    return (avg_callback_us * 100.0f) / profiler->callback_budget_us;
}

int rt_profiler_is_healthy(RTProfiler *profiler) {
    if (!profiler->enabled || profiler->callback_count == 0) {
        return 1;  /* Assume healthy if not profiling */
    }
    
    /* Check CPU usage */
    float cpu_percent = rt_profiler_get_cpu_percent(profiler);
    if (cpu_percent > RT_PROFILER_CRITICAL_LATENCY_PERCENT) {
        return 0;  /* CPU usage too high */
    }
    
    /* Check underruns */
    uint64_t underruns = atomic_load(&profiler->underrun_count);
    if (underruns > 0) {
        return 0;  /* Any underruns = not healthy */
    }
    
    /* Check buffer miss rate */
    uint64_t miss_add = atomic_load(&profiler->buffer_miss_luxstral);
    uint64_t miss_poly = atomic_load(&profiler->buffer_miss_luxsynth);
    uint64_t miss_photo = atomic_load(&profiler->buffer_miss_luxwave);
    uint64_t miss_total = miss_add + miss_poly + miss_photo;
    
    if (miss_total > 0) {
        float miss_rate = (miss_total * 100.0f) / profiler->callback_count;
        if (miss_rate > 2.0f) {  /* > 2% buffer miss rate */
            return 0;
        }
    }
    
    /* Check mutex contention */
    if (profiler->mutex_lock_attempts > 0) {
        float contention_rate = (profiler->mutex_contentions * 100.0f) / profiler->mutex_lock_attempts;
        if (contention_rate > 5.0f) {  /* > 5% contention */
            return 0;
        }
        
        uint64_t avg_mutex_wait = profiler->mutex_total_wait_us / profiler->mutex_lock_attempts;
        if (avg_mutex_wait > RT_PROFILER_WARN_MUTEX_WAIT_US) {
            return 0;  /* Average wait too long */
        }
    }
    
    return 1;  /* All checks passed */
}

void rt_profiler_report_audio_thread_iteration(RTProfiler *profiler, uint64_t elapsed_us) {
    if (!profiler->enabled) return;
    
    atomic_fetch_add(&profiler->audio_thread_total_time_us, elapsed_us);
    atomic_fetch_add(&profiler->audio_thread_iteration_count, 1);
    
    /* Update max time */
    uint64_t current_max = atomic_load(&profiler->audio_thread_max_time_us);
    if (elapsed_us > current_max) {
        atomic_store(&profiler->audio_thread_max_time_us, elapsed_us);
    }
}

void rt_profiler_report_udp_thread_packet(RTProfiler *profiler, uint64_t elapsed_us) {
    if (!profiler->enabled) return;
    
    atomic_fetch_add(&profiler->udp_thread_total_time_us, elapsed_us);
    atomic_fetch_add(&profiler->udp_thread_packet_count, 1);
    
    /* Update max time */
    uint64_t current_max = atomic_load(&profiler->udp_thread_max_time_us);
    if (elapsed_us > current_max) {
        atomic_store(&profiler->udp_thread_max_time_us, elapsed_us);
    }
}
