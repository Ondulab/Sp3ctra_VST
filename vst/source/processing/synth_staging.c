/*
 * synth_staging.c — see synth_staging.h for the contract.
 */
#include "synth_staging.h"
#include "image_chain.h"         /* IMAGE_CHAIN_INSERT_OUT_LUXSYNTH */
#include "config/config_loader.h"
#include "config_instrument.h"   /* CIS_MAX_PIXELS_NB (LuxSynth line staging) */
#include <string.h>

/* Constant-power centre for notes with no pan information. */
#define SYNTH_STAGING_CENTRE_GAIN 0.70710678f

typedef struct {
    /* seqlock: odd = writer inside; readers retry on mismatch/odd. */
    volatile uint32_t seq;
    volatile int      active;
    int               bank_slot;
    int               num_notes;
    int               stereo_valid;
    float             contrast_factor;
    float             notes[PREPROCESS_MAX_NOTES];
    float             left_gains[PREPROCESS_MAX_NOTES];
    float             right_gains[PREPROCESS_MAX_NOTES];
} LsSendStaging;

static LsSendStaging s_ls_staging[CHAIN_MAX_CHAINS];

/* Mixer-side scratch for one slot snapshot (audio thread only). */
static LsSendStaging s_mix_snap;

void synth_staging_init(void)
{
    memset((void*) s_ls_staging, 0, sizeof(s_ls_staging));
}

void synth_staging_stage_luxstral(int chain_idx, int bank_slot,
                                  const PreprocessedImageData* pp,
                                  int num_notes, int stereo_valid)
{
    if (chain_idx < 0 || chain_idx >= CHAIN_MAX_CHAINS || pp == NULL)
        return;
    if (num_notes < 0) num_notes = 0;
    if (num_notes > PREPROCESS_MAX_NOTES) num_notes = PREPROCESS_MAX_NOTES;

    LsSendStaging* s = &s_ls_staging[chain_idx];

    __atomic_store_n(&s->seq, s->seq + 1, __ATOMIC_RELEASE);   /* → odd */
    s->bank_slot       = (bank_slot >= 0 && bank_slot < CHAIN_MAX_CHAINS)
                         ? bank_slot : 0;
    s->num_notes       = num_notes;
    s->stereo_valid    = stereo_valid ? 1 : 0;
    s->contrast_factor = pp->additive.contrast_factor;
    memcpy(s->notes, pp->additive.notes, (size_t) num_notes * sizeof(float));
    if (stereo_valid)
    {
        memcpy(s->left_gains,  pp->stereo.left_gains,
               (size_t) num_notes * sizeof(float));
        memcpy(s->right_gains, pp->stereo.right_gains,
               (size_t) num_notes * sizeof(float));
    }
    s->active = 1;
    __atomic_store_n(&s->seq, s->seq + 1, __ATOMIC_RELEASE);   /* → even */
}

void synth_staging_set_inactive(int chain_idx)
{
    if (chain_idx < 0 || chain_idx >= CHAIN_MAX_CHAINS)
        return;
    LsSendStaging* s = &s_ls_staging[chain_idx];
    __atomic_store_n(&s->seq, s->seq + 1, __ATOMIC_RELEASE);
    s->active = 0;
    __atomic_store_n(&s->seq, s->seq + 1, __ATOMIC_RELEASE);
}

/* Consistent snapshot of one slot (bounded retries; ~40 KB memcpy). Returns 0
 * when the slot is inactive or persistently torn (skip it this frame). */
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
    return 0;
}

int synth_staging_mix_luxstral(const ChainPlan* plan,
                               float* notes_out, int max_notes,
                               float* left_out, float* right_out,
                               float* contrast_out, int* stereo_valid_out)
{
    if (plan == NULL || notes_out == NULL || max_notes <= 0)
        return 0;

    /* Accumulators. left/right accumulate gain·weight·note; normalised at
     * the end by the per-note weighted amplitude. */
    memset(notes_out, 0, (size_t) max_notes * sizeof(float));
    if (left_out)  memset(left_out,  0, (size_t) max_notes * sizeof(float));
    if (right_out) memset(right_out, 0, (size_t) max_notes * sizeof(float));

    float  contrast_acc   = 0.0f;
    float  weight_acc     = 0.0f;
    int    mixed          = 0;
    int    any_stereo     = 0;
    int    out_notes      = 0;

    for (int k = 0; k < plan->num_ls_sends && k < CHAIN_MAX_CHAINS; ++k)
    {
        const LsSendPlan* snd = &plan->ls_send[k];
        if (snd->chain_idx < 0 || snd->chain_idx >= CHAIN_MAX_CHAINS)
            continue;
        if (! staging_snapshot(&s_ls_staging[snd->chain_idx], &s_mix_snap))
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

        contrast_acc += w * s_mix_snap.contrast_factor;
        weight_acc   += w;
        ++mixed;
    }

    if (mixed == 0)
    {
        if (contrast_out)     *contrast_out     = 0.0f;
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

    if (contrast_out)
        *contrast_out = (weight_acc > 0.0f) ? contrast_acc / weight_acc : 0.0f;
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
    __atomic_store_n(&s->seq, s->seq + 1, __ATOMIC_RELEASE);   /* → odd */
    s->bank_slot = (bank_slot >= 0 && bank_slot < CHAIN_MAX_CHAINS)
                   ? bank_slot : 0;
    s->nb_pixels = nb_pixels;
    memcpy(s->line,   line, (size_t) nb_pixels * sizeof(float));
    memcpy(s->rgb[0], r,    (size_t) nb_pixels);
    memcpy(s->rgb[1], g,    (size_t) nb_pixels);
    memcpy(s->rgb[2], b,    (size_t) nb_pixels);
    s->active = 1;
    __atomic_store_n(&s->seq, s->seq + 1, __ATOMIC_RELEASE);   /* → even */
}

void synth_staging_luxsynth_set_inactive(int chain_idx)
{
    if (chain_idx < 0 || chain_idx >= CHAIN_MAX_CHAINS)
        return;
    LxSendStaging* s = &s_lx_staging[chain_idx];
    __atomic_store_n(&s->seq, s->seq + 1, __ATOMIC_RELEASE);
    s->active = 0;
    __atomic_store_n(&s->seq, s->seq + 1, __ATOMIC_RELEASE);
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

        if (! lx_staging_snapshot(&s_lx_staging[c], &s_lx_snap))
            continue;
        gen += __atomic_load_n(&s_lx_staging[c].seq, __ATOMIC_ACQUIRE)
               + (uint32_t) (c * 0x9E3779B9u);

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
