#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARNING = 1,
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_DEBUG = 3
} log_level_t;

// Startup verbosity control
typedef enum {
    STARTUP_VERBOSE_MINIMAL = 0,   // Only errors and final status
    STARTUP_VERBOSE_NORMAL = 1,    // Condensed output (default)
    STARTUP_VERBOSE_FULL = 2       // Full detailed output
} startup_verbose_t;

// Global log level (can be set from environment or config)
extern log_level_t g_log_level;

// Startup verbosity level (controlled by SP3CTRA_STARTUP_VERBOSE env var)
extern startup_verbose_t g_startup_verbose;

// Initialize logger
void logger_init(log_level_t level);

// Check if startup verbose logging is enabled
int is_startup_verbose(void);
int is_startup_full_verbose(void);

// Log functions
void log_error(const char* module, const char* fmt, ...);
void log_warning(const char* module, const char* fmt, ...);
void log_info(const char* module, const char* fmt, ...);
void log_debug(const char* module, const char* fmt, ...);

// Monotonic millisecond clock used by the throttling macros below. Cheap, but
// NOT for the audio thread (all logging is deferred off it anyway).
uint64_t log_monotonic_ms(void);

// Resolved persistent-log-file path (opens it on first call), or NULL if file
// logging is disabled/unavailable. Handy for a one-line "logging to …" notice.
const char* logger_log_file_path(void);

// ============================================================================
// RATE-LIMITED LOGGING
// A recurring fault (e.g. an engine over budget every block) must ALERT without
// itself becoming a firehose. These emit at most once per `ms` window PER CALL
// SITE (state is a function-local static). Message-thread use only. The window
// check races benignly across threads (at worst a couple extra lines).
// ============================================================================
#define LOG_EVERY_MS(_fn, _ms, _module, ...)                                   \
    do {                                                                       \
        static uint64_t _log_last_ms = 0;                                      \
        const uint64_t _log_now_ms = log_monotonic_ms();                       \
        if (_log_now_ms - _log_last_ms >= (uint64_t)(_ms)) {                   \
            _log_last_ms = _log_now_ms;                                        \
            _fn(_module, __VA_ARGS__);                                         \
        }                                                                      \
    } while (0)

#define log_error_every_ms(ms, module, ...)   LOG_EVERY_MS(log_error,   ms, module, __VA_ARGS__)
#define log_warning_every_ms(ms, module, ...) LOG_EVERY_MS(log_warning, ms, module, __VA_ARGS__)
#define log_info_every_ms(ms, module, ...)    LOG_EVERY_MS(log_info,    ms, module, __VA_ARGS__)

// Emit at most once for the whole process lifetime (per call site).
#define LOG_ONCE(_fn, _module, ...)                                            \
    do {                                                                       \
        static int _log_once_done = 0;                                         \
        if (!_log_once_done) { _log_once_done = 1; _fn(_module, __VA_ARGS__); }\
    } while (0)

#define log_warning_once(module, ...) LOG_ONCE(log_warning, module, __VA_ARGS__)
#define log_error_once(module, ...)   LOG_ONCE(log_error,   module, __VA_ARGS__)

// ============================================================================
// STARTUP VERBOSE MACROS
// Use these for init-time logs that should be condensed in normal mode
// Set SP3CTRA_STARTUP_VERBOSE=2 env var for full output
// ============================================================================

// Log only in STARTUP_VERBOSE_FULL mode (detailed init logs)
#define log_startup_detail(module, ...) \
    do { if (is_startup_full_verbose()) log_debug(module, __VA_ARGS__); } while(0)

// Log in STARTUP_VERBOSE_NORMAL and FULL modes (condensed init logs)
#define log_startup_info(module, ...) \
    do { if (is_startup_verbose()) log_info(module, __VA_ARGS__); } while(0)

// Config-specific logging with line numbers
void config_log_error(int line, const char* fmt, ...);
void config_log_warning(int line, const char* fmt, ...);
void config_log_info(int line, const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif // LOGGER_H
