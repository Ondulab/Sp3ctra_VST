/*
 * luxgrain_vst_adapter.c — global LuxGrain instance (see header).
 */

#include "luxgrain_vst_adapter.h"

LuxGrainEngine g_luxgrain_engine;

float g_luxgrain_out_l[LUXGRAIN_MAX_BUFFER_SIZE];
float g_luxgrain_out_r[LUXGRAIN_MAX_BUFFER_SIZE];
