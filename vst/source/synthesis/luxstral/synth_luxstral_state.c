/*
 * synth_luxstral_state.c
 *
 * State management for additive synthesis
 * Contains freeze/fade functionality and display buffer management
 *
 * Author: zhonx
 */

/* Includes ------------------------------------------------------------------*/
#include "vst_adapters_c.h"
#include "synth_luxstral_state.h"
#include "luxstral_engine.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Global variables ----------------------------------------------------------*/

/* Synth Data Freeze Feature - immutable constant (shared by all instances) */
const double G_SYNTH_DATA_FADE_DURATION_SECONDS = 5.0; // Corresponds to visual fade

/* NOTE: freeze/fade flags, frozen grayscale buffer and displayable RGB buffers
 * now live in LuxStralEngine (luxstral_engine.h). The public functions below
 * are thin wrappers operating on the single instance g_luxstral_engine.    */

/* Private function implementations ------------------------------------------*/

// Helper function to get current time in seconds
double synth_getCurrentTimeInSeconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts); // CLOCK_MONOTONIC is usually preferred for time differences
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void synth_data_freeze_init_impl(LuxStralEngine *eng) {
  int nb_pixels = get_cis_pixels_nb();

  if (pthread_mutex_init(&eng->synth_data_freeze_mutex, NULL) != 0) {
    perror("Failed to initialize synth data freeze mutex");
    // Handle error appropriately, e.g., exit or log
    return;
  }

  // Allocate frozen grayscale buffer
  eng->frozen_grayscale_buffer = (float *)calloc(nb_pixels, sizeof(float));
  if (eng->frozen_grayscale_buffer == NULL) {
    perror("Failed to allocate frozen grayscale buffer");
    pthread_mutex_destroy(&eng->synth_data_freeze_mutex);
    return;
  }
}

static void synth_data_freeze_cleanup_impl(LuxStralEngine *eng) {
  pthread_mutex_destroy(&eng->synth_data_freeze_mutex);

  // Free frozen grayscale buffer
  if (eng->frozen_grayscale_buffer != NULL) {
    free(eng->frozen_grayscale_buffer);
    eng->frozen_grayscale_buffer = NULL;
  }
}

static void displayable_synth_buffers_init_impl(LuxStralEngine *eng) {
  int nb_pixels = get_cis_pixels_nb();

  if (pthread_mutex_init(&eng->displayable_synth_mutex, NULL) != 0) {
    perror("Failed to initialize displayable synth data mutex");
    return;
  }

  // Allocate displayable RGB buffers
  eng->displayable_synth_R = (uint8_t *)calloc(nb_pixels, sizeof(uint8_t));
  eng->displayable_synth_G = (uint8_t *)calloc(nb_pixels, sizeof(uint8_t));
  eng->displayable_synth_B = (uint8_t *)calloc(nb_pixels, sizeof(uint8_t));

  if (eng->displayable_synth_R == NULL || eng->displayable_synth_G == NULL || eng->displayable_synth_B == NULL) {
    perror("Failed to allocate displayable synth RGB buffers");
    pthread_mutex_destroy(&eng->displayable_synth_mutex);

    // Clean up any successful allocations
    if (eng->displayable_synth_R != NULL) {
      free(eng->displayable_synth_R);
      eng->displayable_synth_R = NULL;
    }
    if (eng->displayable_synth_G != NULL) {
      free(eng->displayable_synth_G);
      eng->displayable_synth_G = NULL;
    }
    if (eng->displayable_synth_B != NULL) {
      free(eng->displayable_synth_B);
      eng->displayable_synth_B = NULL;
    }
    return;
  }
}

static void displayable_synth_buffers_cleanup_impl(LuxStralEngine *eng) {
  pthread_mutex_destroy(&eng->displayable_synth_mutex);

  // Free displayable RGB buffers
  if (eng->displayable_synth_R != NULL) {
    free(eng->displayable_synth_R);
    eng->displayable_synth_R = NULL;
  }
  if (eng->displayable_synth_G != NULL) {
    free(eng->displayable_synth_G);
    eng->displayable_synth_G = NULL;
  }
  if (eng->displayable_synth_B != NULL) {
    free(eng->displayable_synth_B);
    eng->displayable_synth_B = NULL;
  }
}

/* Public wrappers (signatures unchanged, operate on g_luxstral_engine) ----*/

void synth_data_freeze_init(void) {
  synth_data_freeze_init_impl(&g_luxstral_engine);
}

void synth_data_freeze_cleanup(void) {
  synth_data_freeze_cleanup_impl(&g_luxstral_engine);
}

void displayable_synth_buffers_init(void) {
  displayable_synth_buffers_init_impl(&g_luxstral_engine);
}

void displayable_synth_buffers_cleanup(void) {
  displayable_synth_buffers_cleanup_impl(&g_luxstral_engine);
}

/* Display buffer accessors for external consumers (multithreading.c, UI) ----*/

void luxstral_engine_displayable_lock(void) {
  pthread_mutex_lock(&g_luxstral_engine.displayable_synth_mutex);
}

void luxstral_engine_displayable_unlock(void) {
  pthread_mutex_unlock(&g_luxstral_engine.displayable_synth_mutex);
}

uint8_t *luxstral_engine_displayable_R(void) {
  return g_luxstral_engine.displayable_synth_R;
}

uint8_t *luxstral_engine_displayable_G(void) {
  return g_luxstral_engine.displayable_synth_G;
}

uint8_t *luxstral_engine_displayable_B(void) {
  return g_luxstral_engine.displayable_synth_B;
}
