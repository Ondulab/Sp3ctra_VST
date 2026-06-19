#include "KeyboardRulerComponent.h"

// C engine state — read-only here (display overlay).
extern "C" {
    #include "processing/lux_pitch.h"     // g_lux_pitch_proc
    #include "processing/lux_mask.h"      // g_lux_mask_proc
    #include "config/config_loader.h"     // g_sp3ctra_config.num_octaves
    #include "config/config_instrument.h" // get_cis_pixels_nb()
}

#include <cmath>
#include <cstring>

namespace
{
    constexpr juce::uint32 kAccentPitch = 0xffe06bb8;   // pink  (LuxPitch identity)
    constexpr juce::uint32 kAccentMask  = 0xff6be0d0;   // teal  (LuxMask identity)

    // Strip palette — deliberately subdued so the keys read as a ruler,
    // not as a playable keyboard.
    constexpr juce::uint32 kColStripBg   = 0xff15151c;
    constexpr juce::uint32 kColWhiteKey  = 0xff2a2a34;
    constexpr juce::uint32 kColBlackKey  = 0xff101016;
    constexpr juce::uint32 kColKeyGap    = 0xff15151c;
    constexpr juce::uint32 kColOctLabel  = 0x80aab4c8;

    bool isBlackKey(int note) noexcept
    {
        switch (note % 12) { case 1: case 3: case 6: case 8: case 10: return true;
                             default: return false; }
    }

    const char* const kNoteLetters[12] =
        { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    juce::String midiNoteName(int note)
    {
        // Same octave convention as the reference-note Choice params: C1 = 24.
        return juce::String(kNoteLetters[((note % 12) + 12) % 12])
             + juce::String(note / 12 - 1);
    }
}

//==============================================================================
KeyboardRulerComponent::KeyboardRulerComponent(Sp3ctraAudioProcessor& p)
    : processor(p)
{
    for (int v = 0; v < kMaxVoices; ++v)
        voiceLabelNotes[v] = -1;

    setModule(Module::Pitch);
    setInterceptsMouseClicks(true, false);
}

KeyboardRulerComponent::~KeyboardRulerComponent()
{
    stopTimer();
}

//==============================================================================
void KeyboardRulerComponent::setModule(Module m)
{
    module = m;
    accent = juce::Colour(m == Module::Pitch ? kAccentPitch : kAccentMask);
    setTooltip(juce::String(m == Module::Pitch ? "PITCH" : "MASK")
               + " piano ruler - keys align with the image columns above. "
                 "Alt/Shift-click a key to set the reference note. "
                 "Plain clicks do nothing (display only).");

    lastPps = -1.0f;          // force overlay rebuild + repaint on next tick
    if (isShowing())
    {
        refreshSnapshot();
        rebuildStaticOverlays();
        repaint();
    }
}

//==============================================================================
void KeyboardRulerComponent::visibilityChanged()
{
    if (isVisible())
    {
        refreshSnapshot();
        rebuildStaticOverlays();
        startTimerHz(30);     // overlay refresh — repaints ONLY this strip
        repaint();
    }
    else
    {
        stopTimer();
    }
}

void KeyboardRulerComponent::resized()
{
    lastWidth = -1;           // mapping → x cache depends on the width
    refreshSnapshot();
    rebuildStaticOverlays();
}

//==============================================================================
float KeyboardRulerComponent::pixelToX(float px) const noexcept
{
    return (pixelCount > 0.0f) ? px / pixelCount * (float) getWidth() : 0.0f;
}

float KeyboardRulerComponent::xToPixel(float x) const noexcept
{
    const float w = (float) getWidth();
    return (w > 0.0f) ? x / w * pixelCount : 0.0f;
}

//==============================================================================
void KeyboardRulerComponent::refreshSnapshot()
{
    /* Pixel↔note mapping — MUST mirror lux_pitch.c / lux_mask.c exactly:
     *   pps = (coupling == LUXSTRAL) ? pixel_count / (num_octaves * 12)
     *                                : free_pixels_per_semitone               */
    const int pixN = get_cis_pixels_nb();
    int octaves = g_sp3ctra_config.num_octaves;
    if (octaves <= 0)
        octaves = 8;          // same fallback as the C engines

    pixelCount = (float) pixN;

    /* THREAD-SAFETY NOTE — display-only reads:
     *   • state.config is a plain struct written by the message thread itself
     *     (APVTS → config sync in PluginProcessor), so reading it here (also
     *     message thread) is race-free.
     *   • voices[v].current_pos / current_shift / envelope_level / note are
     *     plain floats/ints written by the synthesis thread.  Reading them
     *     here is a TOLERATED BENIGN RACE: they are aligned word-sized
     *     values, so reads are stale at worst, never invalid — fine for a
     *     30 Hz overlay.  Do NOT use these reads for anything but display. */
    if (module == Module::Pitch)
    {
        const LuxPitchConfig& cfg = g_lux_pitch_proc.config;
        pps = (cfg.coupling_mode == LUX_PITCH_COUPLING_LUXSTRAL)
                  ? (float) pixN / ((float) octaves * 12.0f)
                  : cfg.free_pixels_per_semitone;
        refNote = cfg.reference_note;

        numVoices = cfg.polyphony_enabled ? LUX_PITCH_MAX_VOICES : 1;
        for (int v = 0; v < numVoices; ++v)
        {
            const LuxPitchVoiceState& vs = g_lux_pitch_proc.voices[v];
            voices[v].posPx   = (float) pixN * 0.5f + vs.current_shift;
            voices[v].env     = vs.envelope_level;
            voices[v].widthPx = 0.0f;
            voices[v].note    = vs.note;
        }
    }
    else
    {
        const LuxMaskConfig& cfg = g_lux_mask_proc.config;
        pps = (cfg.coupling_mode == LUX_MASK_COUPLING_LUXSTRAL)
                  ? (float) pixN / ((float) octaves * 12.0f)
                  : cfg.free_pixels_per_semitone;
        refNote = cfg.reference_note;

        numVoices = cfg.polyphony_enabled ? LUX_MASK_MAX_VOICES : 1;
        for (int v = 0; v < numVoices; ++v)
        {
            const LuxMaskVoiceState& vs = g_lux_mask_proc.voices[v];
            voices[v].posPx   = vs.current_pos;
            // Approximate visible width with the configured base width — the
            // exact per-stage bloom width is not required for the overlay.
            voices[v].widthPx = cfg.width_base;
            voices[v].env     = vs.envelope_level;
            voices[v].note    = vs.note;
        }
    }

    for (int v = numVoices; v < kMaxVoices; ++v)
        voices[v] = {};

    // Cache note-name labels outside paint (Strings rebuilt only on change).
    for (int v = 0; v < numVoices; ++v)
    {
        if (voices[v].env > 0.01f && voices[v].note != voiceLabelNotes[v])
        {
            voiceLabels[v]     = midiNoteName(voices[v].note);
            voiceLabelNotes[v] = voices[v].note;
        }
    }
}

//==============================================================================
void KeyboardRulerComponent::rebuildStaticOverlays()
{
    const int W = getWidth();
    octLabels.clear();
    refMarker.clear();
    if (W <= 0 || pps <= 0.01f || pixelCount <= 0.0f)
        return;

    const float centre = pixelCount * 0.5f;

    // Octave labels at every visible C key — only when an octave spans enough
    // horizontal room for the text not to collide with the next label.
    const float octaveSpanX = 12.0f * pps / pixelCount * (float) W;
    if (octaveSpanX >= 24.0f)
    {
        int nLo = refNote + (int) std::floor((0.0f - centre) / pps) - 1;
        int nHi = refNote + (int) std::ceil ((pixelCount - centre) / pps) + 1;
        nLo = juce::jlimit(0, 127, nLo);
        nHi = juce::jlimit(0, 127, nHi);

        for (int n = nLo; n <= nHi; ++n)
        {
            if (n % 12 != 0)
                continue;     // C keys only
            const float x = pixelToX(centre + ((float)(n - refNote) - 0.5f) * pps);
            if (x >= -2.0f && x < (float) W - 12.0f)
                octLabels.push_back({ x, midiNoteName(n) });
        }
    }

    // Reference-note marker: by construction the reference note sits at the
    // image centre (offset 0) — small triangle pointing down + thin line.
    const float refX = pixelToX(centre);
    refMarker.addTriangle(refX - 4.5f, 0.0f, refX + 4.5f, 0.0f, refX, 6.0f);
}

//==============================================================================
void KeyboardRulerComponent::timerCallback()
{
    refreshSnapshot();

    const bool mappingChanged = (pps        != lastPps)
                             || (refNote    != lastRefNote)
                             || (pixelCount != lastPixelCount)
                             || (getWidth() != lastWidth);
    if (mappingChanged)
        rebuildStaticOverlays();

    const bool voicesChanged = (numVoices != lastNumVoices)
        || std::memcmp(voices, lastVoices, sizeof(voices)) != 0;

    if (mappingChanged || voicesChanged)
    {
        lastPps        = pps;
        lastRefNote    = refNote;
        lastPixelCount = pixelCount;
        lastWidth      = getWidth();
        lastNumVoices  = numVoices;
        std::memcpy(lastVoices, voices, sizeof(voices));
        repaint();            // repaints only this 26-px strip
    }
}

//==============================================================================
void KeyboardRulerComponent::paint(juce::Graphics& g)
{
    const int W = getWidth();
    const int H = getHeight();
    if (W <= 0 || H <= 0)
        return;

    g.fillAll(juce::Colour(kColStripBg));

    if (pps > 0.01f && pixelCount > 0.0f)
    {
        const float centre = pixelCount * 0.5f;
        const float fH     = (float) H;
        const float blackH = fH * 0.62f;

        // Visible note range (key boundaries at half-semitone pixel positions)
        int nLo = refNote + (int) std::floor((0.0f - centre) / pps) - 1;
        int nHi = refNote + (int) std::ceil ((pixelCount - centre) / pps) + 1;
        nLo = juce::jlimit(0, 127, nLo);
        nHi = juce::jlimit(0, 127, nHi);

        // White-key base across the keyboard span
        {
            const float xLo = pixelToX(centre + ((float)(nLo - refNote) - 0.5f) * pps);
            const float xHi = pixelToX(centre + ((float)(nHi - refNote) + 0.5f) * pps);
            g.setColour(juce::Colour(kColWhiteKey));
            g.fillRect(juce::jmax(0.0f, xLo), 0.0f,
                       juce::jmin((float) W, xHi) - juce::jmax(0.0f, xLo), fH);
        }

        // Black keys + white/white boundaries (B|C and E|F)
        for (int n = nLo; n <= nHi; ++n)
        {
            const float x0 = pixelToX(centre + ((float)(n - refNote) - 0.5f) * pps);
            const float x1 = pixelToX(centre + ((float)(n - refNote) + 0.5f) * pps);
            if (x1 < 0.0f || x0 > (float) W)
                continue;

            if (isBlackKey(n))
            {
                g.setColour(juce::Colour(kColBlackKey));
                g.fillRect(x0, 0.0f, x1 - x0, blackH);
            }
            else if (n % 12 == 0 || n % 12 == 5)    // C and F start after B / E
            {
                g.setColour(juce::Colour(kColKeyGap));
                g.fillRect(x0, 0.0f, 1.0f, fH);
            }
        }

        // Octave labels (cached strings — no allocation here)
        g.setColour(juce::Colour(kColOctLabel));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
        for (const auto& l : octLabels)
            g.drawText(l.text, (int) l.x + 2, H - 10, 20, 9,
                       juce::Justification::centredLeft, false);
    }

    // ── Reference-note marker (module accent) ────────────────────────────────
    {
        const float refX = pixelToX(pixelCount * 0.5f);
        g.setColour(accent.withAlpha(0.85f));
        g.fillRect(refX - 0.5f, 0.0f, 1.0f, (float) H);
        g.fillPath(refMarker);
    }

    // ── Live voice overlay (opacity = envelope level) ─────────────────────────
    g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
    for (int v = 0; v < numVoices; ++v)
    {
        const VoiceSnap& vs = voices[v];
        if (vs.env <= 0.01f)
            continue;

        const float env = juce::jlimit(0.0f, 1.0f, vs.env);
        const float x   = pixelToX(vs.posPx);

        if (module == Module::Mask)
        {
            // Translucent band ≈ current mask extent around the spot centre
            const float halfW = juce::jmax(1.5f, pixelToX(vs.widthPx) * 0.5f);
            g.setColour(accent.withAlpha(env * 0.28f));
            g.fillRect(x - halfW, 0.0f, halfW * 2.0f, (float) H);
        }

        // Vertical marker at the voice position
        g.setColour(accent.withAlpha(0.25f + env * 0.75f));
        g.fillRect(x - 1.0f, 0.0f, 2.0f, (float) H);

        // Note name when there is room (cached label — no allocation)
        if (voiceLabelNotes[v] == vs.note && x > 16.0f && x < (float) W - 26.0f)
        {
            g.setColour(juce::Colours::white.withAlpha(0.35f + env * 0.6f));
            g.drawText(voiceLabels[v], (int) x + 4, 1, 24, 9,
                       juce::Justification::centredLeft, false);
        }
    }

    // Outer border
    g.setColour(juce::Colour(Sp3ctraTheme::kColBorder));
    g.drawRect(getLocalBounds());
}

//==============================================================================
void KeyboardRulerComponent::mouseUp(const juce::MouseEvent& e)
{
    if (!e.mouseWasClicked())
        return;

    // Display instrument: plain clicks do nothing.  ALT/SHIFT-click on a key
    // sets the module's reference note (Choice param, C1..B6 → MIDI 24..95).
    if (!(e.mods.isAltDown() || e.mods.isShiftDown()))
        return;
    if (pps <= 0.01f || pixelCount <= 0.0f)
        return;

    const float px   = xToPixel((float) e.x);
    const int   note = refNote
        + (int) std::lround((px - pixelCount * 0.5f) / pps);
    const int clamped = juce::jlimit(24, 95, note);     // C1..B6 choice range

    const char* paramId = (module == Module::Pitch) ? "luxpitchReferenceNote"
                                                    : "luxmaskReferenceNote";
    if (auto* param = processor.getAPVTS().getParameter(paramId))
    {
        const float norm = param->convertTo0to1((float)(clamped - 24));
        param->beginChangeGesture();
        param->setValueNotifyingHost(norm);
        param->endChangeGesture();
    }
}
