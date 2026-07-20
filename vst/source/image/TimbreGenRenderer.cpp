#include "TimbreGenRenderer.h"

#include <cmath>

namespace timbregen
{

//==============================================================================
// Presets
//==============================================================================
namespace
{
    struct PresetDef
    {
        const char* name;
        // timbral fields only (see applyPreset)
        int    numPartials;
        double slopeDbPerOct;
        double oddBias;
        double inharmonicity;
        double combDepth, combPos;
        double attackMs, decaySec, hfDamp;
        bool   bellMode;
        int    bellTable;
        double vibCents, vibRateHz, vibOnsetSec, vibLife;
    };

    // Slope/odd values follow the Fourier series of the ideal waveforms
    // (square = odd 1/n → −6 dB/oct odd-only; triangle = odd 1/n² → −12 dB/oct);
    // the acoustic instruments are pragmatic starting points meant to be tweaked.
    // Vibrato defaults belong to the sustained acoustic instruments; the rate/
    // onset/life values stay musical even at depth 0 so raising the depth on
    // any preset sounds alive immediately. New presets are APPENDED — the
    // preset index is persisted (timbreGenState / midiScoreGenState).
    const PresetDef kPresets[] =
    {
        //  name              nPart slope  odd   inharm  comb  pos   atk    dec   hf    bell  table  vibC rate  onset life
        { "Sine",                1,  0.0, 0.0, 0.0,     0.0, 0.28,   5.0,  0.0, 0.0,  false, 0,   0.0, 5.5, 0.40, 0.50 },
        { "Square",             40, -6.0, 1.0, 0.0,     0.0, 0.28,   3.0,  0.0, 0.0,  false, 0,   0.0, 5.5, 0.40, 0.50 },
        { "Triangle",           24, -12.0,1.0, 0.0,     0.0, 0.28,   3.0,  0.0, 0.0,  false, 0,   0.0, 5.5, 0.40, 0.50 },
        { "Sawtooth",           40, -6.0, 0.0, 0.0,     0.0, 0.28,   3.0,  0.0, 0.0,  false, 0,   0.0, 5.5, 0.40, 0.50 },
        { "Organ",               8, -2.5, 0.0, 0.0,     0.0, 0.28,  10.0,  0.0, 0.0,  false, 0,   0.0, 5.5, 0.40, 0.50 },
        { "Clarinet",           20, -8.0, 0.8, 0.0,     0.0, 0.28,  30.0,  0.0, 0.1,  false, 0,   8.0, 5.0, 0.60, 0.50 },
        { "Brass",              30, -3.5, 0.0, 0.0,     0.0, 0.28,  60.0,  0.0, 0.1,  false, 0,  12.0, 4.8, 0.70, 0.55 },
        { "E. Guitar (pluck)",  32, -5.0, 0.0, 3.0e-4,  0.8, 0.13,   2.0,  2.5, 0.7,  false, 0,   0.0, 5.5, 0.40, 0.50 },
        { "E. Piano",           16, -9.0, 0.0, 1.0e-4,  0.3, 0.10,   2.0,  3.5, 0.8,  false, 0,   0.0, 5.5, 0.40, 0.50 },
        { "Bell (church)",      12,  0.0, 0.0, 0.0,     0.0, 0.28,   1.0,  6.0, 0.4,  true,  0,   0.0, 5.5, 0.40, 0.50 },
        { "Bell (glocken)",      4,  0.0, 0.0, 0.0,     0.0, 0.28,   1.0,  3.0, 0.5,  true,  1,   0.0, 5.5, 0.40, 0.50 },
        // Bowed string: harmonic series, bow-position comb (Helmholtz dips),
        // slow bow start, sustained — and the reference singing vibrato.
        { "Violin",             32, -4.5, 0.0, 0.0,     0.35,0.11,  90.0,  0.0, 0.0,  false, 0,  30.0, 5.5, 0.50, 0.65 },
    };

    // Inharmonic partial tables. decayMul > 1 = dies faster than the prime.
    struct BellPartial { double ratio, ampDb, decayMul; };

    // Church bell: hum / prime / tierce (minor third!) / quint / nominal + uppers.
    const BellPartial kChurchBell[] =
    {
        { 0.50,  -3.0, 0.5 }, { 1.00,   0.0, 0.7 }, { 1.20,  -2.0, 1.0 },
        { 1.50,  -9.0, 1.3 }, { 2.00,  -3.0, 1.5 }, { 2.50,  -8.0, 1.8 },
        { 2.67, -12.0, 2.0 }, { 3.00, -11.0, 2.3 }, { 3.70, -14.0, 2.6 },
        { 4.20, -15.0, 3.0 }, { 5.40, -19.0, 3.5 }, { 6.80, -23.0, 4.2 },
    };

    // Ideal struck bar (glockenspiel / vibraphone family).
    const BellPartial kStruckBar[] =
    {
        { 1.00,   0.0, 1.0 }, { 2.76,  -7.0, 1.6 },
        { 5.40, -14.0, 2.5 }, { 8.93, -21.0, 3.6 },
    };
}

int numPresets() { return (int) (sizeof(kPresets) / sizeof(kPresets[0])); }

const char* presetName(int preset)
{
    if (preset < 0 || preset >= numPresets())
        return "Custom";
    return kPresets[preset].name;
}

void applyPreset(TimbreSlotParams& p, int preset)
{
    if (preset < 0 || preset >= numPresets())
        return;
    const PresetDef& d = kPresets[preset];
    p.preset        = preset;
    p.numPartials   = d.numPartials;
    p.slopeDbPerOct = d.slopeDbPerOct;
    p.oddBias       = d.oddBias;
    p.inharmonicity = d.inharmonicity;
    p.combDepth     = d.combDepth;
    p.combPos       = d.combPos;
    p.attackMs      = d.attackMs;
    p.decaySec      = d.decaySec;
    p.hfDamp        = d.hfDamp;
    p.bellMode      = d.bellMode;
    p.bellTable     = d.bellTable;
    p.vibCents      = d.vibCents;
    p.vibRateHz     = d.vibRateHz;
    p.vibOnsetSec   = d.vibOnsetSec;
    p.vibLife       = d.vibLife;
}

//==============================================================================
// Partial computation
//==============================================================================
double midiNoteHz(int midiNote)
{
    return 440.0 * std::pow(2.0, (midiNote - 69) / 12.0);
}

juce::String midiNoteLabel(int midiNote)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F",
                                   "F#", "G", "G#", "A", "A#", "B" };
    const int octave = midiNote / 12 - 1;   // MIDI 60 = C4
    return juce::String(names[midiNote % 12]) + juce::String(octave)
         + " (" + juce::String(midiNoteHz(midiNote), 1) + " Hz)";
}

std::vector<Partial> computePartials(const TimbreSlotParams& p)
{
    std::vector<Partial> out;
    const double f0 = midiNoteHz(p.midiNote);

    if (p.bellMode)
    {
        const BellPartial* table = (p.bellTable == 1) ? kStruckBar : kChurchBell;
        const int n = (p.bellTable == 1)
                        ? (int) (sizeof(kStruckBar)  / sizeof(kStruckBar[0]))
                        : (int) (sizeof(kChurchBell) / sizeof(kChurchBell[0]));
        out.reserve((size_t) n);
        for (int i = 0; i < n; ++i)
            out.push_back({ f0 * table[i].ratio, table[i].ampDb, table[i].decayMul });
        return out;
    }

    const int N = juce::jlimit(1, 64, p.numPartials);
    out.reserve((size_t) N);
    for (int n = 1; n <= N; ++n)
    {
        // Stiff-string stretch: f_n = n·f0·√(1 + B·n²)
        const double stretch = std::sqrt(1.0 + p.inharmonicity * (double) n * n);
        const double f = f0 * n * stretch;

        double db = p.slopeDbPerOct * std::log2((double) n);
        if ((n % 2) == 0)
            db += -48.0 * p.oddBias;                    // even harmonics fade out
        if (p.combDepth > 0.0)
        {
            // Pluck-position comb: amplitude ∝ |sin(π·n·pos)|, scaled by depth.
            const double comb = std::abs(std::sin(juce::MathConstants<double>::pi
                                                  * n * p.combPos));
            db += p.combDepth * 20.0 * std::log10(juce::jmax(comb, 1.0e-3));
        }

        // Decay-rate multiplier: upper partials die faster with hfDamp.
        const double decayMul = 1.0 + p.hfDamp * (n - 1) * 0.35;
        out.push_back({ f, db, decayMul });
    }
    return out;
}

//==============================================================================
// Page rendering
//==============================================================================
namespace
{
    inline double mmToPx(double mm, double dpi) { return mm * dpi / 25.4; }
}

double slotSeconds(const TimbrePageSettings& s)
{
    if (s.writingSpeed <= 0.0)
        return 0.0;
    const double labelMarginMM = 150.0 * 25.4 / 400.0;   // same left margin as SCORE
    const double bandWidthMM   = SCORE_A4_WIDTH_MM - labelMarginMM;
    const double slotWidthMM   = (bandWidthMM - (kNumSlots - 1) * s.slotGapMM)
                               / (double) kNumSlots;
    return (slotWidthMM / 10.0) / s.writingSpeed;        // mm→cm, cm / (cm/s)
}

scoregen::RenderResult renderTimbrePage(
    const std::array<TimbreSlotParams, kNumSlots>& slots,
    const TimbrePageSettings& settings)
{
    scoregen::RenderResult result;
    auto fail = [&](const juce::String& msg) -> scoregen::RenderResult
    {
        result.ok  = false;
        result.log = msg;
        return result;
    };

    const double dpi = (settings.printerDpi >= 72.0) ? settings.printerDpi
                                                     : SCORE_DEFAULT_PRINTER_DPI;
    if (settings.minFreq <= 0.0 || settings.maxFreq <= settings.minFreq)
        return fail("Invalid frequency range");
    if (settings.writingSpeed <= 0.0)
        return fail("Writing speed must be > 0");

    bool anyEnabled = false;
    for (const auto& s : slots) anyEnabled |= s.enabled;
    if (! anyEnabled)
        return fail("No sound enabled (activate at least one slot)");

    // ── Page geometry (A4 portrait, same band placement as SCORE) ────────────
    const int imageW = (int) mmToPx(SCORE_A4_WIDTH_MM,  dpi);
    const int imageH = (int) mmToPx(SCORE_A4_HEIGHT_MM, dpi);

    const double labelMargin    = 150.0 * (dpi / 400.0);        // SCORE's left margin
    const double bottomMarginPx = mmToPx(settings.bottomMarginMM,  dpi);
    const double spectroHeightPx= mmToPx(settings.spectroHeightMM, dpi);
    const double spectroLeft    = labelMargin;
    const double spectroBottom  = imageH - bottomMarginPx;
    const double spectroTop     = spectroBottom - spectroHeightPx;
    const double spectroWidth   = imageW - labelMargin;
    if (spectroTop < 0.0 || spectroWidth <= 0.0)
        return fail("Band does not fit the page at these margins");

    const double gapPx   = mmToPx(settings.slotGapMM, dpi);
    const double slotW   = (spectroWidth - (kNumSlots - 1) * gapPx) / (double) kNumSlots;
    if (slotW < 8.0)
        return fail("Slot width degenerate (gap too large)");

    const double pxPerSec = (dpi / 2.54) * settings.writingSpeed;
    const double slotSec  = slotW / pxPerSec;

    // ── Page-wide normalisation: strongest partial of any enabled slot = 0 dB ─
    // (envelope peaks at 0, so the loudest printed cell hits full black exactly
    // like SCORE's global_max normalisation).
    std::array<std::vector<Partial>, kNumSlots> partials;
    double pageMaxDb = -1.0e9;
    for (int i = 0; i < kNumSlots; ++i)
    {
        if (! slots[(size_t) i].enabled) continue;
        partials[(size_t) i] = computePartials(slots[(size_t) i]);
        for (const auto& pt : partials[(size_t) i])
            pageMaxDb = juce::jmax(pageMaxDb, pt.ampDb + slots[(size_t) i].levelDb);
    }
    if (pageMaxDb < -1.0e8)
        return fail("No partial to draw");

    // ── White page ────────────────────────────────────────────────────────────
    juce::Image img(juce::Image::RGB, imageW, imageH, true);
    {
        juce::Graphics g(img);
        g.fillAll(juce::Colours::white);
    }

    const int yTop = juce::jmax(0,      (int) std::floor(spectroTop));
    const int yBot = juce::jmin(imageH, (int) std::ceil (spectroBottom));

    const double range     = juce::jmax(1.0, settings.dynamicRangeDB);
    const double logRatio  = std::log(settings.maxFreq / settings.minFreq);
    const double halfWidth = juce::jmax(0.5, mmToPx(settings.lineWidthMM, dpi) * 0.5);
    const int    dyMax     = (int) std::ceil(halfWidth * 2.0);   // Gaussian skirt

    juce::Image::BitmapData bmp(img, juce::Image::BitmapData::readWrite);
    auto darken = [&](int x, int y, double intensity)   // 0 = black … 1 = white
    {
        if (x < 0 || x >= imageW || y < yTop || y >= yBot) return;
        const auto v = (juce::uint8) juce::jlimit(0, 255,
                            (int) (intensity * 255.0 + 0.5));
        juce::uint8* p = bmp.getLinePointer(y) + x * bmp.pixelStride;
        if (v < p[0]) p[0] = p[1] = p[2] = v;            // darker (louder) wins
    };

    // ── Draw each enabled slot ────────────────────────────────────────────────
    const double fadeSec = juce::jmin(0.15 / settings.writingSpeed,   // 1.5 mm
                                      slotSec * 0.10);
    juce::StringArray logLines;

    for (int si = 0; si < kNumSlots; ++si)
    {
        const TimbreSlotParams& sp = slots[(size_t) si];
        if (! sp.enabled) continue;

        const double slotX0 = spectroLeft + si * (slotW + gapPx);
        const int    x0     = (int) std::floor(slotX0);
        const int    x1     = (int) std::ceil (slotX0 + slotW);
        const double attSec = juce::jmax(1.0e-4, sp.attackMs / 1000.0);

        for (const auto& pt : partials[(size_t) si])
        {
            if (pt.freqHz < settings.minFreq || pt.freqHz > settings.maxFreq)
                continue;   // outside the instrument's span — like off-page ink

            // LOG frequency axis: same mapping as SCORE / the reader.
            const double pos = std::log(pt.freqHz / settings.minFreq) / logRatio;
            const double yC  = spectroBottom - pos * spectroHeightPx;

            const double baseDb = (pt.ampDb + sp.levelDb) - pageMaxDb;   // ≤ 0
            if (baseDb <= -range)
                continue;   // below the printable dynamic window everywhere

            // Vertical Gaussian cross-profile (soft-edged line, decodes like an
            // FFT peak): −12 dB at ±halfWidth, computed once per row offset.
            const int yCi = (int) std::round(yC);
            for (int x = x0; x < x1; ++x)
            {
                const double t = (x - slotX0) / pxPerSec;

                double envDb = 0.0;
                if (t < attSec)                                     // attack ramp
                    envDb += 20.0 * std::log10(juce::jmax(t / attSec, 1.0e-4));
                if (sp.decaySec > 0.0 && t > attSec)                // −60 dB decay
                    envDb += -60.0 * (t - attSec) / sp.decaySec * pt.decayMul;
                const double tail = slotSec - t;                    // end fade (anti-click
                if (tail < fadeSec)                                 // + strip separation)
                    envDb += 20.0 * std::log10(juce::jmax(tail / fadeSec, 1.0e-4));

                const double dB = baseDb + envDb;
                if (dB <= -range)
                    continue;

                for (int dy = -dyMax; dy <= dyMax; ++dy)
                {
                    const double d   = ((yCi + dy) - yC) / halfWidth;
                    const double off = -12.0 * d * d;               // Gaussian in dB
                    const double v   = juce::jlimit(0.0, 1.0, -(dB + off) / range);
                    if (v < 1.0)
                        darken(x, yCi + dy, v);
                }
            }
        }
    }

    // ── Margins: cut marks (always) + optional writings in the TOP margin ────
    {
        juce::Graphics g(img);

        // Cut marks at the slot edges, above and below the band — needed to
        // slice the six strips, so they are always drawn (no text involved).
        for (int si = 1; si < kNumSlots; ++si)
        {
            const double slotX0 = spectroLeft + si * (slotW + gapPx);
            const float cx  = (float) (slotX0 - gapPx * 0.5);
            const float len = (float) mmToPx(3.0, dpi);
            g.setColour(juce::Colour(0xff909090));
            g.drawLine(cx, (float) spectroTop - len,    cx, (float) spectroTop,    1.0f);
            g.drawLine(cx, (float) spectroBottom,       cx, (float) spectroBottom + len, 1.0f);
        }

        // Writings are OPT-IN and live at the very top of the page, far from
        // the scanned band (a plain print stays clean by default).
        if (settings.showLabels)
        {
            const float labelH = (float) mmToPx(4.0, dpi);
            g.setFont(juce::FontOptions(labelH * 0.72f));

            for (int si = 0; si < kNumSlots; ++si)
            {
                const TimbreSlotParams& sp = slots[(size_t) si];
                const double slotX0 = spectroLeft + si * (slotW + gapPx);

                g.setColour(sp.enabled ? juce::Colours::black
                                       : juce::Colour(0xffb0b0b0));
                const juce::String name = (sp.preset >= 0 && sp.preset < numPresets())
                                            ? presetName(sp.preset) : "Custom";
                // Compact note label ("A3 220Hz") — the full form overflows a slot.
                const juce::String note = midiNoteLabel(sp.midiNote)
                                              .upToFirstOccurrenceOf(" (", false, false)
                                        + " " + juce::String(midiNoteHz(sp.midiNote), 0) + "Hz";
                g.drawText(juce::String(si + 1) + ". " + name + "  " + note,
                           juce::Rectangle<float>((float) slotX0,
                                                  (float) mmToPx(2.0, dpi),
                                                  (float) slotW, labelH),
                           juce::Justification::centredLeft);
            }

            // Footer: the settings needed to reproduce / play the print in tune —
            // one line under the titles, still in the top margin.
            g.setColour(juce::Colour(0xff707070));
            g.setFont(juce::FontOptions((float) mmToPx(2.6, dpi)));
            g.drawText("Sp3ctra TIMBRE  |  " + juce::String(settings.minFreq, 0) + "-"
                           + juce::String(settings.maxFreq, 0) + " Hz log  |  band "
                           + juce::String(settings.spectroHeightMM, 3) + " mm  |  "
                           + juce::String(settings.writingSpeed, 1) + " cm/s  |  "
                           + juce::String(slotSec, 2) + " s/sound  |  "
                           + juce::String(dpi, 0) + juce::String::fromUTF8(" DPI — print at 100%"),
                       juce::Rectangle<float>((float) spectroLeft,
                                              (float) mmToPx(6.5, dpi),
                                              (float) spectroWidth, (float) mmToPx(3.5, dpi)),
                       juce::Justification::centredLeft);
        }
    }

    // ── Result ────────────────────────────────────────────────────────────────
    logLines.add("Page: " + juce::String(imageW) + " x " + juce::String(imageH)
                 + " px @ " + juce::String(dpi, 0) + " DPI (A4 portrait)");
    logLines.add("Band: " + juce::String(settings.minFreq, 0) + "-"
                 + juce::String(settings.maxFreq, 0) + " Hz, "
                 + juce::String(settings.spectroHeightMM, 3) + " mm");
    logLines.add(juce::String(kNumSlots) + " slots, "
                 + juce::String(slotSec, 2) + " s each @ "
                 + juce::String(settings.writingSpeed, 1) + " cm/s");

    result.image       = img;
    result.ok          = true;
    result.stereo      = false;
    result.pixelWidth  = imageW;
    result.pixelHeight = imageH;
    result.spectroBand = juce::Rectangle<int>(
        (int) std::floor(spectroLeft), yTop,
        juce::jmax(1, (int) std::ceil(spectroLeft + spectroWidth) - (int) std::floor(spectroLeft)),
        juce::jmax(1, yBot - yTop));
    result.log = logLines.joinIntoString("\n");
    return result;
}

} // namespace timbregen
