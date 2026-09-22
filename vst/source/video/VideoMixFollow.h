/**
 * @file VideoMixFollow.h
 * @brief THE audio-follow law of the VIDEO MIX radar — shared by the toile
 *        (ui/VideoMixRadar.h) and the compositor (VideoMixerComponent's
 *        render thread), exactly like the projector's law next door
 *        (VideoMixFocus.h).
 *
 * ONE gesture for both mixes. The link runs in ONE direction at a time
 * (kDirId) — two masks facing each other, never both at once (that would
 * be a feedback loop):
 *
 *   → AUDIO  the TOILE drives the sound: the PROJECTOR is the audio mix of
 *            the chains on the toile — a CROSSFADE, not a mask
 *            (VideoMixFocus::audioWeight). At the centre every chain sits
 *            at 50 % of its send faders; pulled onto a spoke, that chain
 *            alone is heard at 100 % and every other one is silent; in
 *            between, the straight blend. The handles play no part and
 *            neither does the depth below — the audio faders do not move,
 *            they stay the ceiling, one hand does the rest.
 *   → VIDEO  the AUDIO MIX drives the picture: each output takes the level
 *            its chain has in the audio mix.
 *
 * The → VIDEO direction below. Armed, every VIDEO SCROLL output
 * takes the level its CHAIN has in the audio mix. It is a SECOND MASK,
 * multiplied into the same product as the projector's:
 *
 *   eff_i = level_i × weight_focus(i) × weight_follow(i)
 *
 * and it obeys the same contract — it can only MASK, never invent. The
 * polygon the user drew stays the ceiling.
 *
 * The reading is DIRECT, not relative: the audio level of a chain IS the
 * weight (dosed by depth). A send fader at 30 % puts its image at 30 % of
 * the level the toile gives it — move an audio fader and the picture
 * answers, one for one. (A relative reading, each chain against the
 * loudest, was tried first: with a flat audio mix every weight came out at
 * 1 and the whole feature was invisible.)
 *
 *   weight_i = 1 − depth · (1 − lvl_i)      depth 0 = neutral, 1 = one-for-one
 *
 * Two things it must never do:
 *   • black out a chain that feeds NO engine (kExempt): it is not
 *     "unheard", it simply takes no part in the audio conversation — a
 *     purely visual chain must not vanish when the follow is armed;
 *   • in LIVE, collapse everything because nothing happens to be sounding:
 *     with no signal at all the mode falls back on the SET levels.
 *
 * The audio levels come from Sp3ctraAudioProcessor::chainAudioLevels() —
 * the AUDIO MIX truth — in two flavours (kModeId):
 *   SET  — the mix SETTINGS alone: send level × power/solo × engine volume,
 *          a plain 0…1 fader position. Deterministic: it moves when a hand,
 *          a CC or an automation moves.
 *   LIVE — the same, further dimmed by how much the chain is ACTUALLY
 *          sounding (its send meter against the loudest one on the toile),
 *          so a chain that goes quiet fades out of the picture.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <cmath>

namespace VideoMixFollow
{
    /** The four global APVTS ids (not banked — one follow for the toile). */
    inline constexpr const char* kArmId   = "videoMixFollow";       ///< bool
    inline constexpr const char* kDepthId = "videoMixFollowDepth";  ///< 0…1 (→ VIDEO only)
    inline constexpr const char* kModeId  = "videoMixFollowMode";   ///< 0 SET, 1 LIVE
    inline constexpr const char* kDirId   = "videoMixFollowDir";    ///< 0 →VIDEO, 1 →AUDIO

    /** Below this, a level counts as silence. */
    inline constexpr float kSilence = 1.0e-4f;

    /** The level a chain reports when it feeds no engine at all — exempt. */
    inline constexpr float kExempt = -1.0f;

    /** Envelope of the followed level: quick to light up, slow to let go, so
     *  a busy mix does not strobe the image. */
    inline constexpr float kAttackMs  = 70.0f;
    inline constexpr float kReleaseMs = 420.0f;

    /** LIVE flavour: the SET level, dimmed by how much this chain is really
     *  sounding — its meter against the loudest meter on the toile. With no
     *  signal anywhere it falls back on the SET level (never a black
     *  screen), and an exempt chain stays exempt. */
    inline float liveLevel(float setLvl, float meter, float peakMeter) noexcept
    {
        if (setLvl <= kExempt + 1.0e-6f) return setLvl;   // exempt
        if (peakMeter <= kSilence)       return setLvl;   // nothing sounds
        return setLvl * juce::jlimit(0.0f, 1.0f, meter / peakMeter);
    }

    /** The mask itself (0…1, 1 = untouched) — the third factor of eff_i.
     *  `lvl` = the chain's audio level, or kExempt. */
    inline float weight(float lvl, float depth) noexcept
    {
        if (lvl <= kExempt + 1.0e-6f) return 1.0f;        // exempt: never masked
        const float d = juce::jlimit(0.0f, 1.0f, depth);
        return juce::jlimit(0.0f, 1.0f, 1.0f - d * (1.0f - juce::jlimit(0.0f, 1.0f, lvl)));
    }

    /** dt-aware one-pole toward `target` (kAttackMs up, kReleaseMs down). */
    inline float smooth(float cur, float target, float dtMs) noexcept
    {
        const float tau = target > cur ? kAttackMs : kReleaseMs;
        const float a   = 1.0f - std::exp(-juce::jmax(0.0f, dtMs) / juce::jmax(1.0f, tau));
        const float v   = cur + a * (target - cur);
        return std::abs(target - v) < 1.0e-4f ? target : v;
    }

    /** The value that goes into the compositor's change-detection signature.
     *  A weight that follows the audio moves at every frame; quantized, a
     *  steady mix over a frozen output still yields an identical signature —
     *  which is what keeps a paused waterfall from republishing 60 times a
     *  second (VideoMixerComponent::Renderer::renderFrame). */
    inline float quantized(float w) noexcept
    {
        return std::round(juce::jlimit(0.0f, 1.0f, w) * 128.0f);
    }
} // namespace VideoMixFollow
