/* logger.c */

#include "logger.h"
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

log_level_t g_log_level = LOG_LEVEL_DEBUG;
startup_verbose_t g_startup_verbose = STARTUP_VERBOSE_NORMAL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_colors_enabled = -1; // -1 = not initialized, 0 = disabled, 1 = enabled
static int g_startup_verbose_initialized = 0;

// ── Persistent log file (plain text, colourless, rotated) ───────────────────
// Every line that reaches stderr is also appended here so a session's faults
// (perf warnings, dropouts, errors) survive the app closing and can be read
// after the fact. Path: $SP3CTRA_LOG_FILE, else ~/Library/Logs/Sp3ctra (macOS)
// / ~/.local/state/Sp3ctra (other). Disable with SP3CTRA_NO_LOG_FILE=1.
// Single size-based rotation: at the cap the file becomes ".1" and a fresh one
// opens (so at most ~2× the cap on disk). All access is under g_log_mutex.
#define LOG_FILE_MAX_BYTES (8 * 1024 * 1024)
static FILE* g_log_file = NULL;
static long  g_log_file_bytes = 0;
static int   g_log_file_state = -1;   // -1 unknown, 0 enabled, 1 disabled
static char  g_log_file_path[1024] = {0};

// ANSI color codes
#define ANSI_RESET          "\033[0m"
#define ANSI_BOLD           "\033[1m"
#define ANSI_DIM            "\033[2m"

// Level colors
#define ANSI_RED            "\033[91m"  // Bright red for errors
#define ANSI_YELLOW         "\033[93m"  // Bright yellow for warnings
#define ANSI_CYAN           "\033[96m"  // Bright cyan for info
#define ANSI_GRAY           "\033[90m"  // Gray for debug

// Module colors (using different hues for visual differentiation)
#define ANSI_MODULE_BLUE    "\033[94m"  // Bright blue
#define ANSI_MODULE_MAGENTA "\033[95m"  // Bright magenta
#define ANSI_MODULE_GREEN   "\033[92m"  // Bright green
#define ANSI_MODULE_YELLOW  "\033[93m"  // Bright yellow
#define ANSI_MODULE_CYAN    "\033[96m"  // Bright cyan
#define ANSI_MODULE_WHITE   "\033[97m"  // Bright white

// Time color
#define ANSI_TIME_DIM       "\033[2;37m" // Dim white for timestamp

static void init_startup_verbose(void) {
    if (g_startup_verbose_initialized) return;
    
    const char* env = getenv("SP3CTRA_STARTUP_VERBOSE");
    if (env != NULL) {
        int val = atoi(env);
        if (val == 0) {
            g_startup_verbose = STARTUP_VERBOSE_MINIMAL;
        } else if (val == 2) {
            g_startup_verbose = STARTUP_VERBOSE_FULL;
        } else {
            g_startup_verbose = STARTUP_VERBOSE_NORMAL;
        }
    } else {
        g_startup_verbose = STARTUP_VERBOSE_NORMAL;
    }
    
    g_startup_verbose_initialized = 1;
}

static void init_colors(void) {
    if (g_colors_enabled != -1) return;
    
    // Check if NO_COLOR or SP3CTRA_NO_COLOR environment variable is set
    if (getenv("NO_COLOR") != NULL || getenv("SP3CTRA_NO_COLOR") != NULL) {
        g_colors_enabled = 0;
        return;
    }
    
    // Check if stderr is a terminal
    g_colors_enabled = isatty(STDERR_FILENO) ? 1 : 0;
}

static const char* get_color_for_level(log_level_t level) {
    if (!g_colors_enabled) return "";
    
    switch (level) {
        case LOG_LEVEL_ERROR:   return ANSI_RED;
        case LOG_LEVEL_WARNING: return ANSI_YELLOW;
        case LOG_LEVEL_INFO:    return ANSI_CYAN;
        case LOG_LEVEL_DEBUG:   return ANSI_GRAY;
        default:                return "";
    }
}

static const char* get_color_for_module(const char* module) {
    if (!g_colors_enabled) return "";
    
    // Simple hash-based color selection for consistent module colors
    unsigned int hash = 0;
    for (const char* p = module; *p; p++) {
        hash = hash * 31 + (unsigned char)*p;
    }
    
    // Map to one of 6 distinct colors
    switch (hash % 6) {
        case 0: return ANSI_MODULE_BLUE;
        case 1: return ANSI_MODULE_MAGENTA;
        case 2: return ANSI_MODULE_GREEN;
        case 3: return ANSI_MODULE_YELLOW;
        case 4: return ANSI_MODULE_CYAN;
        case 5: return ANSI_MODULE_WHITE;
        default: return ANSI_MODULE_WHITE;
    }
}

static const char* get_reset_code(void) {
    return g_colors_enabled ? ANSI_RESET : "";
}

static const char* get_time_color(void) {
    return g_colors_enabled ? ANSI_TIME_DIM : "";
}

void logger_init(log_level_t level) {
    g_log_level = level;
    init_colors();
    init_startup_verbose();
}

int is_startup_verbose(void) {
    init_startup_verbose();
    return (g_startup_verbose >= STARTUP_VERBOSE_NORMAL);
}

int is_startup_full_verbose(void) {
    init_startup_verbose();
    return (g_startup_verbose == STARTUP_VERBOSE_FULL);
}

static const char* level_string(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_ERROR:   return "ERROR";
        case LOG_LEVEL_WARNING: return "WARNING";
        case LOG_LEVEL_INFO:    return "INFO";
        case LOG_LEVEL_DEBUG:   return "DEBUG";
        default:                return "UNKNOWN";
    }
}

// Create every missing directory along `dir` (best-effort, errors ignored).
static void log_mkdir_p(const char* dir) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", dir);
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

// Resolve the log-file path once into g_log_file_path.
static void log_build_path(void) {
    const char* env = getenv("SP3CTRA_LOG_FILE");
    if (env != NULL && env[0] != '\0') {
        snprintf(g_log_file_path, sizeof g_log_file_path, "%s", env);
        return;
    }
#ifdef _WIN32
    // %LOCALAPPDATA%\Sp3ctra\Logs — forward slashes on purpose: Windows path
    // APIs accept them and log_open_file's mkdir walk splits on '/'.
    const char* base = getenv("LOCALAPPDATA");
    if (base == NULL || base[0] == '\0')
        base = getenv("USERPROFILE");
    if (base == NULL || base[0] == '\0') {
        snprintf(g_log_file_path, sizeof g_log_file_path, "C:/Temp/Sp3ctra.log");
        return;
    }
    snprintf(g_log_file_path, sizeof g_log_file_path,
             "%s/Sp3ctra/Logs/Sp3ctra.log", base);
#else
    const char* home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        snprintf(g_log_file_path, sizeof g_log_file_path, "/tmp/Sp3ctra.log");
        return;
    }
#ifdef __APPLE__
    snprintf(g_log_file_path, sizeof g_log_file_path,
             "%s/Library/Logs/Sp3ctra/Sp3ctra.log", home);
#else
    snprintf(g_log_file_path, sizeof g_log_file_path,
             "%s/.local/state/Sp3ctra/Sp3ctra.log", home);
#endif
#endif
}

// Rename `path` → `path.1`, dropping any previous backup.
static void log_rotate_file(void) {
    char bak[1100];
    snprintf(bak, sizeof bak, "%s.1", g_log_file_path);
    rename(g_log_file_path, bak);   // best-effort; overwrites old .1
}

// Return the open log file (opening it lazily), or NULL if disabled/unopenable.
// Caller must hold g_log_mutex. Never logs (would deadlock on the mutex).
static FILE* log_file_get_locked(void) {
    if (g_log_file_state == -1)
        g_log_file_state = (getenv("SP3CTRA_NO_LOG_FILE") != NULL) ? 1 : 0;
    if (g_log_file_state == 1)
        return NULL;

    if (g_log_file == NULL) {
        log_build_path();
        // Rotate a large log left by a previous session before appending.
        struct stat st;
        if (stat(g_log_file_path, &st) == 0 && st.st_size > LOG_FILE_MAX_BYTES)
            log_rotate_file();

        // Ensure the parent directory exists.
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", g_log_file_path);
        char* slash = strrchr(dir, '/');
        if (slash != NULL) { *slash = '\0'; log_mkdir_p(dir); }

        g_log_file = fopen(g_log_file_path, "a");
        if (g_log_file == NULL) {
            g_log_file_state = 1;   // give up quietly, stderr still works
            return NULL;
        }
        fseek(g_log_file, 0, SEEK_END);
        g_log_file_bytes = ftell(g_log_file);
        g_log_file_bytes += fprintf(g_log_file,
            "\n========== Sp3ctra session start ==========\n");
    }
    return g_log_file;
}

static void log_message(log_level_t level, const char* module, const char* fmt, va_list args) {
    if (level > g_log_level) return;

    init_colors();

    // Format the message body ONCE — it feeds both the coloured stderr line and
    // the plain file line (a va_list can only be consumed once).
    char body[4096];
    vsnprintf(body, sizeof body, fmt, args);

    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    char ts[16];
    snprintf(ts, sizeof ts, "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);

    const char* level_str    = level_string(level);
    const char* level_color  = get_color_for_level(level);
    const char* module_color = get_color_for_module(module);
    const char* time_color   = get_time_color();
    const char* reset        = get_reset_code();

    pthread_mutex_lock(&g_log_mutex);

    // Coloured line to stderr (unchanged appearance).
    fprintf(stderr, "%s[%s]%s %s[%s]%s %s[%s]%s %s\n",
            time_color, ts, reset,
            level_color, level_str, reset,
            module_color, module, reset,
            body);
    fflush(stderr);

    // Plain, colourless mirror to the rotating log file.
    FILE* lf = log_file_get_locked();
    if (lf != NULL) {
        int n = fprintf(lf, "[%s] [%s] [%s] %s\n", ts, level_str, module, body);
        if (n > 0) g_log_file_bytes += n;
        fflush(lf);
        if (g_log_file_bytes > LOG_FILE_MAX_BYTES) {
            fclose(g_log_file);
            g_log_file = NULL;
            log_rotate_file();
            (void) log_file_get_locked();   // reopen fresh
        }
    }

    pthread_mutex_unlock(&g_log_mutex);
}

void log_error(const char* module, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(LOG_LEVEL_ERROR, module, fmt, args);
    va_end(args);
}

void log_warning(const char* module, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(LOG_LEVEL_WARNING, module, fmt, args);
    va_end(args);
}

void log_info(const char* module, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(LOG_LEVEL_INFO, module, fmt, args);
    va_end(args);
}

void log_debug(const char* module, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(LOG_LEVEL_DEBUG, module, fmt, args);
    va_end(args);
}

void config_log_error(int line, const char* fmt, ...) {
    if (LOG_LEVEL_ERROR > g_log_level) return;
    
    init_colors();
    
    pthread_mutex_lock(&g_log_mutex);
    
    const char* level_color = get_color_for_level(LOG_LEVEL_ERROR);
    const char* module_color = get_color_for_module("CONFIG");
    const char* reset = get_reset_code();
    
    fprintf(stderr, "%s[CONFIG ERROR]%s %sLine %d:%s ", 
            level_color, reset, module_color, line, reset);
    
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    fflush(stderr);
    pthread_mutex_unlock(&g_log_mutex);
}

void config_log_warning(int line, const char* fmt, ...) {
    if (LOG_LEVEL_WARNING > g_log_level) return;
    
    init_colors();
    
    pthread_mutex_lock(&g_log_mutex);
    
    const char* level_color = get_color_for_level(LOG_LEVEL_WARNING);
    const char* module_color = get_color_for_module("CONFIG");
    const char* reset = get_reset_code();
    
    fprintf(stderr, "%s[CONFIG WARNING]%s %sLine %d:%s ", 
            level_color, reset, module_color, line, reset);
    
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    fflush(stderr);
    pthread_mutex_unlock(&g_log_mutex);
}

uint64_t log_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

const char* logger_log_file_path(void) {
    pthread_mutex_lock(&g_log_mutex);
    FILE* lf = log_file_get_locked();   // resolves path + opens on first call
    const char* p = (lf != NULL && g_log_file_path[0] != '\0') ? g_log_file_path : NULL;
    pthread_mutex_unlock(&g_log_mutex);
    return p;
}

void config_log_info(int line, const char* fmt, ...) {
    if (LOG_LEVEL_INFO > g_log_level) return;
    
    init_colors();
    
    pthread_mutex_lock(&g_log_mutex);
    
    const char* level_color = get_color_for_level(LOG_LEVEL_INFO);
    const char* module_color = get_color_for_module("CONFIG");
    const char* reset = get_reset_code();
    
    fprintf(stderr, "%s[CONFIG INFO]%s %sLine %d:%s ", 
            level_color, reset, module_color, line, reset);
    
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    fflush(stderr);
    pthread_mutex_unlock(&g_log_mutex);
}
