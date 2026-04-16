/*
 * voice_manager.c
 *
 * Generic polyphonic voice management — RT-safe, engine-agnostic.
 * Ported from legacy voice_manager.c.
 */

#include "voice_manager.h"

/* ============================================================================
 * voice_manager_allocate
 * ========================================================================== */

int voice_manager_allocate(void *voices, int num_voices,
                           GetVoiceMetadataFn get_metadata,
                           SetVoiceNoteFn set_note,
                           int midi_note,
                           unsigned long long trigger_order)
{
    if (!voices || num_voices <= 0 || !get_metadata) return -1;

    /* Priority 1: Find an IDLE voice */
    for (int i = 0; i < num_voices; i++)
    {
        VoiceMetadata meta = get_metadata(voices, i);
        if (meta.adsr_state == ADSR_STATE_IDLE)
            return i;
    }

    /* Priority 2: Steal RELEASE voice with lowest output (< -26dB first) */
    int best_release = -1;
    float min_release_output = 2.0f;
    for (int i = 0; i < num_voices; i++)
    {
        VoiceMetadata meta = get_metadata(voices, i);
        if (meta.adsr_state == ADSR_STATE_RELEASE && meta.adsr_output < min_release_output)
        {
            min_release_output = meta.adsr_output;
            best_release = i;
        }
    }
    if (best_release >= 0)
        return best_release;

    /* Priority 3: Steal ACTIVE voice with oldest trigger order (LRU) */
    int oldest_idx = -1;
    unsigned long long oldest_order = (unsigned long long)-1;
    for (int i = 0; i < num_voices; i++)
    {
        VoiceMetadata meta = get_metadata(voices, i);
        if (meta.trigger_order < oldest_order)
        {
            oldest_order = meta.trigger_order;
            oldest_idx = i;
        }
    }

    if (oldest_idx >= 0)
        return oldest_idx;

    /* Fallback: voice 0 */
    return 0;
}

/* ============================================================================
 * voice_manager_release
 * ========================================================================== */

int voice_manager_release(void *voices, int num_voices,
                          GetVoiceMetadataFn get_metadata,
                          GetVoiceStateFn get_state,
                          SetVoiceNoteFn set_note,
                          int midi_note)
{
    if (!voices || num_voices <= 0 || !get_metadata) return -1;

    /* Priority 1: Find oldest ACTIVE voice with matching note */
    int best_idx = -1;
    unsigned long long oldest = (unsigned long long)-1;

    for (int i = 0; i < num_voices; i++)
    {
        VoiceMetadata meta = get_metadata(voices, i);
        if (meta.midi_note == midi_note &&
            meta.adsr_state != ADSR_STATE_IDLE &&
            meta.adsr_state != ADSR_STATE_RELEASE)
        {
            if (meta.trigger_order < oldest)
            {
                oldest = meta.trigger_order;
                best_idx = i;
            }
        }
    }

    if (best_idx >= 0)
        return best_idx;

    /* Priority 2: Find RELEASE voice with matching note (late Note Off) */
    for (int i = 0; i < num_voices; i++)
    {
        VoiceMetadata meta = get_metadata(voices, i);
        if (meta.midi_note == midi_note && meta.adsr_state == ADSR_STATE_RELEASE)
        {
            if (set_note) set_note(voices, i, -1);
            return -1; /* Already releasing */
        }
    }

    /* Priority 3: Find IDLE voice with matching note (very late Note Off) */
    for (int i = 0; i < num_voices; i++)
    {
        VoiceMetadata meta = get_metadata(voices, i);
        if (meta.midi_note == midi_note && meta.adsr_state == ADSR_STATE_IDLE)
        {
            if (set_note) set_note(voices, i, -1);
            return -1; /* Already idle */
        }
    }

    return -1; /* Note not found */
}

/* ============================================================================
 * voice_manager_cleanup_idle
 * ========================================================================== */

void voice_manager_cleanup_idle(void *voices, int num_voices,
                                GetVoiceMetadataFn get_metadata,
                                SetVoiceNoteFn set_note)
{
    if (!voices || num_voices <= 0 || !get_metadata || !set_note) return;

    for (int i = 0; i < num_voices; i++)
    {
        VoiceMetadata meta = get_metadata(voices, i);
        if (meta.adsr_state == ADSR_STATE_IDLE && meta.midi_note >= 0)
            set_note(voices, i, -1);
    }
}
