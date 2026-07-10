/*
 * synth_staging.c — see synth_staging.h for the contract.
 */
#include "synth_staging.h"
#include "config/config_loader.h"
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
