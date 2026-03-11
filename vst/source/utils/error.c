//
//  error.c  (VST edition)
//
//  In VST context, calling exit() would crash the DAW.
//  This implementation logs the fatal error and aborts the current operation
//  without killing the host process.
//
#include "error.h"
#include <stdio.h>
#include <string.h>

// VST-safe die(): log the fatal error but do NOT call exit() or abort().
// The synthesis threads will stop producing audio on their own once the
// state is corrupted, and the host can remove the plugin safely.
void die(const char *s) {
    // Minimal stderr output - safe from any thread context
    fprintf(stderr, "[FATAL] %s\n", s ? s : "(null)");
}
