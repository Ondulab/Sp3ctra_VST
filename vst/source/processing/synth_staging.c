/*
 * synth_staging.c — see synth_staging.h for the contract.
 */
#include "synth_staging.h"
#include "image_chain.h"         /* IMAGE_CHAIN_INSERT_OUT_LUXSYNTH */
#include "config/config_loader.h"
#include "config_instrument.h"   /* CIS_MAX_PIXELS_NB (LuxSynth line staging) */
#include <stdint.h>
#include <string.h>

/* Constant-power centre for notes with no pan information. */
#define SYNTH_STAGING_CENTRE_GAIN 0.70710678f

/* Contention diagnostics — a mixer tick found a slot persistently torn
 * (writer mid-staging on every retry) and HELD its previous output instead
 * of mixing. RT threads only bump the counter; the message thread drains it
 * (PluginProcessor timer). */
static volatile uint64_t s_contention_holds = 0;

uint64_t synth_staging_contention_holds(void)
{
    return __atomic_load_n(&s_contention_holds, __ATOMIC_RELAXED);
}

typedef struct {
    /* seqlock: odd = writer inside; readers retry on mismatch/odd.
     * seq moves ONLY by atomic fetch_add: writers on one slot are normally
     * exclusive (chain arbitration), but a transient overlap (ownership
     * handoff, message-thread set_inactive) must not lose an increment — a
     * plain seq++ race left seq odd with no writer inside, and the mixer
     * held its previous output forever (2026-07-24 audio freeze). */
    volatile uint32_t seq;
    volatile int      active;
    int               bank_slot;
    int               num_notes;
    int               stereo_valid;
    float             notes[PREPROCESS_MAX_NOTES];
    float             left_gains[PREPROCESS_MAX_NOTES];
    float             right_gains[PREPROCESS_MAX_NOTES];
} LsSendStaging;

static LsSendStaging s_ls_staging[CHAIN_MAX_CHAINS];

/* Mixer-side scratch for one slot snapshot (audio thread only). */
static LsSendStaging s_mix_snap;

void synth_staging_stage_luxstral(int chain_idx, int bank_slot,
                                  const PreprocessedImageData* pp,
                                  int num_notes, int stereo_valid)
{
    if (chain_idx < 0 || chain_idx >= CHAIN_MAX_CHAINS || pp == NULL)
        return;
    if (num_notes < 0) num_notes = 0;
    if (num_notes > PREPROCESS_MAX_NOTES) num_notes = PREPROCESS_MAX_NOTES;

    LsSendStaging* s = &s_ls_staging[chain_idx];

    __atomic_fetch_add(&s->seq, 1, __ATOMIC_ACQ_REL);          /* → odd */
    s->bank_slot       = (bank_slot >= 0 && bank_slot < CHAIN_MAX_CHAINS)
                         ? bank_slot : 0;
    s->num_notes       = num_notes;
    s->stereo_valid    = stereo_valid ? 1 : 0;
    memcpy(s->notes, pp->additive.notes, (size_t) num_notes * sizeof(float));
    if (stereo_valid)
    {
        memcpy(s->left_gains,  pp->stereo.left_gains,
               (size_t) num_notes * sizeof(float));
        memcpy(s->right_gains, pp->stereo.right_gains,
               (size_t) num_notes * sizeof(float));
    }
    s->active = 1;
    __atomic_fetch_add(&s->seq, 1, __ATOMIC_RELEASE);          /* → even */
}

void synth_staging_set_inactive(int chain_idx)
{
    if (chain_idx < 0 || chain_idx >= CHAIN_MAX_CHAINS)
        return;
    LsSendStaging* s = &s_ls_staging[chain_idx];
    __atomic_fetch_add(&s->seq, 1, __ATOMIC_ACQ_REL);
    s->active = 0;
    __atomic_fetch_add(&s->seq, 1, __ATOMIC_ACQ_REL);
}

/* Consistent snapshot of one slot (bounded retries; ~40 KB memcpy).
 * Returns 1 = data copied, 0 = slot inactive, -1 = persistently torn (the
 * writer was mid-staging on every retry — the retries are back-to-back ns
 * loads, a single ~µs staging memcpy outlasts all of them). A -1 must make
 * the caller HOLD its previous output: treating it as "inactive" turned a
 * transient race into an audible dropout (spectrum/mix slammed to silence)
 * every time the audio tick collided with device line-rate staging. */
static int staging_snapshot(const LsSendStaging* s, LsSendStaging* out)
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const uint32_t s0 = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (s0 & 1u)
            continue;                       /* writer inside — retry */
        if (! s->active)
            return 0;
        memcpy(out, (const void*) s, sizeof(*out));
        const uint32_t s1 = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (s0 == s1)
            return out->active;
    }
    return -1;
}

int synth_staging_mix_luxstral(const ChainPlan* plan,
                               float* notes_out, int max_notes,
                               float* left_out, float* right_out,
                               int* stereo_valid_out)
{
    if (plan == NULL || notes_out == NULL || max_notes <= 0)
        return 0;

    /* Accumulators. left/right accumulate gain·weight·note; normalised at
     * the end by the per-note weighted amplitude. */
    memset(notes_out, 0, (size_t) max_notes * sizeof(float));
    if (left_out)  memset(left_out,  0, (size_t) max_notes * sizeof(float));
    if (right_out) memset(right_out, 0, (size_t) max_notes * sizeof(float));

    int    mixed          = 0;
    int    any_stereo     = 0;
    int    out_notes      = 0;
    int    contended      = 0;

    for (int k = 0; k < plan->num_ls_sends && k < CHAIN_MAX_CHAINS; ++k)
    {
        const LsSendPlan* snd = &plan->ls_send[k];
        if (snd->chain_idx < 0 || snd->chain_idx >= CHAIN_MAX_CHAINS)
            continue;
        const int snap =
            staging_snapshot(&s_ls_staging[snd->chain_idx], &s_mix_snap);
        if (snap < 0) { contended = 1; break; }
        if (snap == 0)
            continue;

        /* Weight = the send's bank intensity, gated by its per-send power.
         * Read the bank stored at STAGE time (follows the producer's plan). */
        const lux_out_params_t* bank =
            &g_sp3ctra_config.luxstral_out[s_mix_snap.bank_slot];
        if (! bank->enabled)
            continue;
        float w = bank->intensity;
        if (w <= 0.0f)
            continue;

        const int n = s_mix_snap.num_notes < max_notes ? s_mix_snap.num_notes
                                                       : max_notes;
        if (n > out_notes) out_notes = n;

        if (s_mix_snap.stereo_valid && left_out && right_out)
        {
            any_stereo = 1;
            for (int i = 0; i < n; ++i)
            {
                const float a = w * s_mix_snap.notes[i];
                notes_out[i] += a;
                left_out[i]  += a * s_mix_snap.left_gains[i];
                right_out[i] += a * s_mix_snap.right_gains[i];
            }
        }
        else
        {
            for (int i = 0; i < n; ++i)
            {
                const float a = w * s_mix_snap.notes[i];
                notes_out[i] += a;
                if (left_out)  left_out[i]  += a * SYNTH_STAGING_CENTRE_GAIN;
                if (right_out) right_out[i] += a * SYNTH_STAGING_CENTRE_GAIN;
            }
        }

        ++mixed;
    }

    /* A torn slot means a producer was mid-staging RIGHT NOW — fresh data is
     * a µs away. HOLD the previous commit (return -1) instead of mixing a
     * partial set or, worse, committing silence: that conflation was the
     * audible micro-dropout under device line-rate staging. */
    if (contended)
    {
        __atomic_fetch_add(&s_contention_holds, 1, __ATOMIC_RELAXED);
        if (stereo_valid_out) *stereo_valid_out = 0;
        return -1;
    }

    if (mixed == 0)
    {
        if (stereo_valid_out) *stereo_valid_out = 0;
        return 0;
    }

    /* Normalise pan gains by the note's weighted amplitude (silent note →
     * constant-power centre so a fading note never slams to a channel). */
    if (left_out && right_out)
    {
        for (int i = 0; i < out_notes; ++i)
        {
            const float a = notes_out[i];
            if (a > 1.0e-9f)
            {
                left_out[i]  /= a;
                right_out[i] /= a;
            }
            else
            {
                left_out[i]  = SYNTH_STAGING_CENTRE_GAIN;
                right_out[i] = SYNTH_STAGING_CENTRE_GAIN;
            }
        }
        /* Notes past the longest send stay centred. */
        for (int i = out_notes; i < max_notes; ++i)
        {
            left_out[i]  = SYNTH_STAGING_CENTRE_GAIN;
            right_out[i] = SYNTH_STAGING_CENTRE_GAIN;
        }
    }

    if (stereo_valid_out)
        *stereo_valid_out = any_stereo;
    return mixed;
}

/* ══ M4 — LuxSynth sends (conditioned line + raw RGB) ══════════════════════ */

typedef struct {
    volatile uint32_t seq;      /* seqlock: odd = writer inside */
    volatile int      active;
    int               bank_slot;
    int               nb_pixels;
    float             line[CIS_MAX_PIXELS_NB];
    uint8_t           rgb[3][CIS_MAX_PIXELS_NB];
} LxSendStaging;

static LxSendStaging s_lx_staging[CHAIN_MAX_CHAINS];
static LxSendStaging s_lx_snap;   /* consumer-side snapshot (audio thread) */

void synth_staging_stage_luxsynth(int chain_idx, int bank_slot,
                                  const float* line,
                                  const uint8_t* r, const uint8_t* g,
                                  const uint8_t* b, int nb_pixels)
{
    if (chain_idx < 0 || chain_idx >= CHAIN_MAX_CHAINS
        || line == NULL || r == NULL || g == NULL || b == NULL)
        return;
    if (nb_pixels < 0) nb_pixels = 0;
    if (nb_pixels > CIS_MAX_PIXELS_NB) nb_pixels = CIS_MAX_PIXELS_NB;

    LxSendStaging* s = &s_lx_staging[chain_idx];
    __atomic_fetch_add(&s->seq, 1, __ATOMIC_ACQ_REL);          /* → odd */
    s->bank_slot = (bank_slot >= 0 && bank_slot < CHAIN_MAX_CHAINS)
                   ? bank_slot : 0;
    s->nb_pixels = nb_pixels;
    memcpy(s->line,   line, (size_t) nb_pixels * sizeof(float));
    memcpy(s->rgb[0], r,    (size_t) nb_pixels);
    memcpy(s->rgb[1], g,    (size_t) nb_pixels);
    memcpy(s->rgb[2], b,    (size_t) nb_pixels);
    s->active = 1;
    __atomic_fetch_add(&s->seq, 1, __ATOMIC_RELEASE);          /* → even */
}

void synth_staging_luxsynth_set_inactive(int chain_idx)
{
    if (chain_idx < 0 || chain_idx >= CHAIN_MAX_CHAINS)
        return;
    LxSendStaging* s = &s_lx_staging[chain_idx];
    __atomic_fetch_add(&s->seq, 1, __ATOMIC_ACQ_REL);
    s->active = 0;
    __atomic_fetch_add(&s->seq, 1, __ATOMIC_ACQ_REL);
}

static int lx_staging_snapshot(const LxSendStaging* s, LxSendStaging* out)
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const uint32_t s0 = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (s0 & 1u)
            continue;
        if (! s->active)
            return 0;
        memcpy(out, (const void*) s, sizeof(*out));
        const uint32_t s1 = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (s0 == s1)
            return out->active;
    }
    return 0;
}

int synth_staging_mix_luxsynth(const ChainPlan* plan,
                               float* line_out,
                               uint8_t* r_out, uint8_t* g_out, uint8_t* b_out,
                               int max_pixels, int* nb_pixels_out,
                               uint32_t* generation_out)
{
    if (plan == NULL || line_out == NULL || max_pixels <= 0)
        return 0;

    memset(line_out, 0, (size_t) max_pixels * sizeof(float));

    /* RGB accumulators (weighted average — colour quality, not energy). */
    static float s_rgb_acc[3][CIS_MAX_PIXELS_NB];   /* audio thread only */
    if (r_out) memset(s_rgb_acc, 0, sizeof(s_rgb_acc));

    float    weight_acc = 0.0f;
    uint32_t gen        = 0;
    int      mixed      = 0;
    int      out_px     = 0;
    int      contended  = 0;

    for (int c = 0; c < plan->num_chains && c < CHAIN_MAX_CHAINS; ++c)
    {
        /* Only chains whose CURRENT plan recipe carries an OUT_LUXSYNTH send
         * contribute — a stale staging from a removed OUT never leaks in. */
        const SynthChainPlan* sp = &plan->chain[c];
        if (! sp->present) continue;
        int has_lx = 0;
        for (int i = 0; i < sp->num_inserts && ! has_lx; ++i)
            if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXSYNTH)
                has_lx = 1;
        if (! has_lx) continue;

        const int snap = lx_staging_snapshot(&s_lx_staging[c], &s_lx_snap);
        if (snap < 0) { contended = 1; break; }
        if (snap == 0)
            continue;
        /* Generation from the SNAPPED seq — consistent with the copied line
         * (a live re-read could see the next write's odd value). */
        gen += s_lx_snap.seq + (uint32_t) (c * 0x9E3779B9u);

        const lux_out_params_t* bank =
            &g_sp3ctra_config.luxsynth_out[s_lx_snap.bank_slot];
        if (! bank->enabled) continue;
        float w = bank->intensity;
        if (w <= 0.0f) continue;

        const int n = s_lx_snap.nb_pixels < max_pixels ? s_lx_snap.nb_pixels
                                                       : max_pixels;
        if (n > out_px) out_px = n;
        for (int i = 0; i < n; ++i)
            line_out[i] += w * s_lx_snap.line[i];
        if (r_out)
            for (int i = 0; i < n; ++i)
            {
                s_rgb_acc[0][i] += w * (float) s_lx_snap.rgb[0][i];
                s_rgb_acc[1][i] += w * (float) s_lx_snap.rgb[1][i];
                s_rgb_acc[2][i] += w * (float) s_lx_snap.rgb[2][i];
            }
        weight_acc += w;
        ++mixed;
    }

    /* Torn slot → HOLD (see mix_luxstral): the engine keeps its spectrum
     * for one tick; silence here was the audible LuxSynth micro-dropout. */
    if (contended)
    {
        __atomic_fetch_add(&s_contention_holds, 1, __ATOMIC_RELAXED);
        if (nb_pixels_out)   *nb_pixels_out = 0;
        if (generation_out)  *generation_out = 0;
        return -1;
    }

    if (mixed == 0)
    {
        if (nb_pixels_out)   *nb_pixels_out = 0;
        if (generation_out)  *generation_out = 0;
        return 0;
    }

    for (int i = 0; i < out_px; ++i)
    {
        if (line_out[i] < 0.0f) line_out[i] = 0.0f;
        if (line_out[i] > 1.0f) line_out[i] = 1.0f;
    }
    if (r_out && g_out && b_out && weight_acc > 0.0f)
    {
        const float inv = 1.0f / weight_acc;
        for (int i = 0; i < out_px; ++i)
        {
            float rv = s_rgb_acc[0][i] * inv;
            float gv = s_rgb_acc[1][i] * inv;
            float bv = s_rgb_acc[2][i] * inv;
            r_out[i] = (uint8_t) (rv < 0.0f ? 0.0f : (rv > 255.0f ? 255.0f : rv));
            g_out[i] = (uint8_t) (gv < 0.0f ? 0.0f : (gv > 255.0f ? 255.0f : gv));
            b_out[i] = (uint8_t) (bv < 0.0f ? 0.0f : (bv > 255.0f ? 255.0f : bv));
        }
    }

    if (nb_pixels_out)  *nb_pixels_out  = out_px;
    if (generation_out) *generation_out = gen;
    return mixed;
}

/* ══ M5 — LuxWave sends (conditioned line, bipolar mix) ════════════════════ */

typedef struct {
    volatile uint32_t seq;
    volatile int      active;
    int               bank_slot;
    int               nb_pixels;
    float             line[CIS_MAX_PIXELS_NB];
} LwSendStaging;

static LwSendStaging s_lw_staging[CHAIN_MAX_CHAINS];
static LwSendStaging s_lw_snap;   /* consumer-side snapshot (audio thread) */

void synth_staging_stage_luxwave(int chain_idx, int bank_slot,
                                 const float* line, int nb_pixels)
{
    if (chain_idx < 0 || chain_idx >= CHAIN_MAX_CHAINS || line == NULL)
        return;
    if (nb_pixels < 0) nb_pixels = 0;
    if (nb_pixels > CIS_MAX_PIXELS_NB) nb_pixels = CIS_MAX_PIXELS_NB;

    LwSendStaging* s = &s_lw_staging[chain_idx];
    __atomic_fetch_add(&s->seq, 1, __ATOMIC_ACQ_REL);
    s->bank_slot = (bank_slot >= 0 && bank_slot < CHAIN_MAX_CHAINS)
                   ? bank_slot : 0;
    s->nb_pixels = nb_pixels;
    memcpy(s->line, line, (size_t) nb_pixels * sizeof(float));
    s->active = 1;
    __atomic_fetch_add(&s->seq, 1, __ATOMIC_ACQ_REL);
}

void synth_staging_luxwave_set_inactive(int chain_idx)
{
    if (chain_idx < 0 || chain_idx >= CHAIN_MAX_CHAINS)
        return;
    LwSendStaging* s = &s_lw_staging[chain_idx];
    __atomic_fetch_add(&s->seq, 1, __ATOMIC_ACQ_REL);
    s->active = 0;
    __atomic_fetch_add(&s->seq, 1, __ATOMIC_ACQ_REL);
}

/* Tri-state like staging_snapshot: 1 = data, 0 = inactive, -1 = torn (hold). */
static int lw_staging_snapshot(const LwSendStaging* s, LwSendStaging* out)
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const uint32_t s0 = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (s0 & 1u)
            continue;
        if (! s->active)
            return 0;
        memcpy(out, (const void*) s, sizeof(*out));
        const uint32_t s1 = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (s0 == s1)
            return out->active;
    }
    return -1;
}

int synth_staging_mix_luxwave(const ChainPlan* plan,
                              float* line_out, int max_pixels,
                              int* nb_pixels_out)
{
    if (plan == NULL || line_out == NULL || max_pixels <= 0)
        return 0;

    /* Bipolar accumulation around the wavetable midpoint. */
    for (int i = 0; i < max_pixels; ++i)
        line_out[i] = 0.5f;

    int mixed     = 0;
    int out_px    = 0;
    int contended = 0;

    for (int c = 0; c < plan->num_chains && c < CHAIN_MAX_CHAINS; ++c)
    {
        const SynthChainPlan* sp = &plan->chain[c];
        if (! sp->present) continue;
        int has_lw = 0;
        for (int i = 0; i < sp->num_inserts && ! has_lw; ++i)
            if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXWAVE)
                has_lw = 1;
        if (! has_lw) continue;

        const int snap = lw_staging_snapshot(&s_lw_staging[c], &s_lw_snap);
        if (snap < 0) { contended = 1; break; }
        if (snap == 0)
            continue;

        const lux_out_params_t* bank =
            &g_sp3ctra_config.luxwave_out[s_lw_snap.bank_slot];
        if (! bank->enabled) continue;
        float w = bank->intensity;
        if (w < 0.0f) w = 0.0f;
        if (w <= 0.0f) { ++mixed; continue; }   /* silent send still counts */

        const int n = s_lw_snap.nb_pixels < max_pixels ? s_lw_snap.nb_pixels
                                                       : max_pixels;
        if (n > out_px) out_px = n;
        for (int i = 0; i < n; ++i)
            line_out[i] += w * (s_lw_snap.line[i] - 0.5f);
        ++mixed;
    }

    /* Torn slot → HOLD (the wavetable keeps its last content, as on 0). */
    if (contended)
    {
        __atomic_fetch_add(&s_contention_holds, 1, __ATOMIC_RELAXED);
        if (nb_pixels_out) *nb_pixels_out = 0;
        return -1;
    }

    if (mixed == 0)
    {
        if (nb_pixels_out) *nb_pixels_out = 0;
        return 0;
    }

    for (int i = 0; i < out_px; ++i)
    {
        if (line_out[i] < 0.0f) line_out[i] = 0.0f;
        if (line_out[i] > 1.0f) line_out[i] = 1.0f;
    }
    if (nb_pixels_out) *nb_pixels_out = (out_px > 0 ? out_px : max_pixels);
    return mixed;
}

/* ══ LuxGrain sends (conditioned line, unipolar mix + generation) ══════════ */

typedef struct {
    volatile uint32_t seq;
    volatile int      active;
    int               bank_slot;
    int               nb_pixels;
    float             line[CIS_MAX_PIXELS_NB];
    uint8_t           rgb[3][CIS_MAX_PIXELS_NB];  /* raw stream (grain pan) */
} LgSendStaging;

static LgSendStaging s_lg_staging[CHAIN_MAX_CHAINS];
static LgSendStaging s_lg_snap;   /* consumer-side snapshot (audio thread) */

void synth_staging_stage_luxgrain(int chain_idx, int bank_slot,
                                  const float* line,
                                  const uint8_t* r, const uint8_t* g,
                                  const uint8_t* b, int nb_pixels)
{
    if (chain_idx < 0 || chain_idx >= CHAIN_MAX_CHAINS || line == NULL)
        return;
    if (nb_pixels < 0) nb_pixels = 0;
    if (nb_pixels > CIS_MAX_PIXELS_NB) nb_pixels = CIS_MAX_PIXELS_NB;

    LgSendStaging* s = &s_lg_staging[chain_idx];
    __atomic_fetch_add(&s->seq, 1, __ATOMIC_ACQ_REL);
    s->bank_slot = (bank_slot >= 0 && bank_slot < CHAIN_MAX_CHAINS)
                   ? bank_slot : 0;
    s->nb_pixels = nb_pixels;
    memcpy(s->line, line, (size_t) nb_pixels * sizeof(float));
    if (r && g && b)
    {
        memcpy(s->rgb[0], r, (size_t) nb_pixels);
        memcpy(s->rgb[1], g, (size_t) nb_pixels);
        memcpy(s->rgb[2], b, (size_t) nb_pixels);
    }
    else
    {
        memset(s->rgb[0], 0, (size_t) nb_pixels);
        memset(s->rgb[1], 0, (size_t) nb_pixels);
        memset(s->rgb[2], 0, (size_t) nb_pixels);
    }
    s->active = 1;
    __atomic_fetch_add(&s->seq, 1, __ATOMIC_ACQ_REL);
}

void synth_staging_luxgrain_set_inactive(int chain_idx)
{
    if (chain_idx < 0 || chain_idx >= CHAIN_MAX_CHAINS)
        return;
    LgSendStaging* s = &s_lg_staging[chain_idx];
    __atomic_fetch_add(&s->seq, 1, __ATOMIC_ACQ_REL);
    s->active = 0;
    __atomic_fetch_add(&s->seq, 1, __ATOMIC_ACQ_REL);
}

static int lg_staging_snapshot(const LgSendStaging* s, LgSendStaging* out)
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const uint32_t s0 = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (s0 & 1u)
            continue;
        if (! s->active)
            return 0;
        memcpy(out, (const void*) s, sizeof(*out));
        const uint32_t s1 = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (s0 == s1)
            return out->active;
    }
    return -1;
}

int synth_staging_mix_luxgrain(const ChainPlan* plan,
                               float* line_out,
                               uint8_t* r_out, uint8_t* g_out, uint8_t* b_out,
                               int max_pixels, int* nb_pixels_out,
                               uint32_t* generation_out)
{
    if (plan == NULL || line_out == NULL || max_pixels <= 0)
        return 0;

    memset(line_out, 0, (size_t) max_pixels * sizeof(float));

    /* RGB accumulators (weighted average — colour quality, not energy). */
    static float s_lg_rgb_acc[3][CIS_MAX_PIXELS_NB];   /* audio thread only */
    if (r_out) memset(s_lg_rgb_acc, 0, sizeof(s_lg_rgb_acc));

    float    weight_acc = 0.0f;
    uint32_t gen       = 0;
    int      mixed     = 0;
    int      out_px    = 0;
    int      contended = 0;

    for (int c = 0; c < plan->num_chains && c < CHAIN_MAX_CHAINS; ++c)
    {
        /* Only chains whose CURRENT plan recipe carries an OUT_LUXGRAIN send
         * contribute — a stale staging from a removed OUT never leaks in. */
        const SynthChainPlan* sp = &plan->chain[c];
        if (! sp->present) continue;
        int has_lg = 0;
        for (int i = 0; i < sp->num_inserts && ! has_lg; ++i)
            if (sp->insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXGRAIN)
                has_lg = 1;
        if (! has_lg) continue;

        const int snap = lg_staging_snapshot(&s_lg_staging[c], &s_lg_snap);
        if (snap < 0) { contended = 1; break; }
        if (snap == 0)
            continue;
        /* Generation from the SNAPPED seq (see mix_luxsynth). */
        gen += s_lg_snap.seq + (uint32_t) (c * 0x9E3779B9u);

        const lux_out_params_t* bank =
            &g_sp3ctra_config.luxgrain_out[s_lg_snap.bank_slot];
        if (! bank->enabled) continue;
        float w = bank->intensity;
        if (w <= 0.0f) { ++mixed; continue; }   /* silent send still counts */

        const int n = s_lg_snap.nb_pixels < max_pixels ? s_lg_snap.nb_pixels
                                                       : max_pixels;
        if (n > out_px) out_px = n;
        for (int i = 0; i < n; ++i)
            line_out[i] += w * s_lg_snap.line[i];
        if (r_out)
            for (int i = 0; i < n; ++i)
            {
                s_lg_rgb_acc[0][i] += w * (float) s_lg_snap.rgb[0][i];
                s_lg_rgb_acc[1][i] += w * (float) s_lg_snap.rgb[1][i];
                s_lg_rgb_acc[2][i] += w * (float) s_lg_snap.rgb[2][i];
            }
        weight_acc += w;
        ++mixed;
    }

    /* Torn slot → HOLD (the engine keeps its ring, as everywhere else). */
    if (contended)
    {
        __atomic_fetch_add(&s_contention_holds, 1, __ATOMIC_RELAXED);
        if (nb_pixels_out)  *nb_pixels_out = 0;
        if (generation_out) *generation_out = 0;
        return -1;
    }

    if (mixed == 0)
    {
        if (nb_pixels_out)  *nb_pixels_out = 0;
        if (generation_out) *generation_out = 0;
        return 0;
    }

    for (int i = 0; i < out_px; ++i)
    {
        if (line_out[i] < 0.0f) line_out[i] = 0.0f;
        if (line_out[i] > 1.0f) line_out[i] = 1.0f;
    }
    if (r_out && g_out && b_out && weight_acc > 0.0f)
    {
        const float inv = 1.0f / weight_acc;
        for (int i = 0; i < out_px; ++i)
        {
            float rv = s_lg_rgb_acc[0][i] * inv;
            float gv = s_lg_rgb_acc[1][i] * inv;
            float bv = s_lg_rgb_acc[2][i] * inv;
            r_out[i] = (uint8_t) (rv < 0.0f ? 0.0f : (rv > 255.0f ? 255.0f : rv));
            g_out[i] = (uint8_t) (gv < 0.0f ? 0.0f : (gv > 255.0f ? 255.0f : gv));
            b_out[i] = (uint8_t) (bv < 0.0f ? 0.0f : (bv > 255.0f ? 255.0f : bv));
        }
    }
    if (nb_pixels_out)  *nb_pixels_out = (out_px > 0 ? out_px : max_pixels);
    if (generation_out) *generation_out = gen;
    return mixed;
}
