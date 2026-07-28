#include "MidiScoreGenRenderer.h"

#include <cmath>

namespace midiscoregen
{

//==============================================================================
// MIDI parsing
//==============================================================================
MidiScoreData parseMidiFile(const juce::File& file)
{
    MidiScoreData d;
    d.sourcePath = file.getFullPathName();

    if (! file.existsAsFile())
    {
        d.error = "File not found: " + file.getFileName();
        return d;
    }

    juce::FileInputStream in(file);
    if (! in.openedOk())
    {
        d.error = "Cannot open " + file.getFileName();
        return d;
    }

    juce::MidiFile mf;
    if (! mf.readFrom(in) || mf.getNumTracks() <= 0)
    {
        d.error = "Not a valid MIDI file: " + file.getFileName();
        return d;
    }
    mf.convertTimestampTicksToSeconds();   // tempo map applied once, here

    // ── Pass 1: which tracks carry notes, and their names ────────────────────
    const int nTracks = mf.getNumTracks();
    std::vector<int>          noteTracks;
    std::vector<juce::String> trackNames((size_t) nTracks);
    for (int t = 0; t < nTracks; ++t)
    {
        const auto* seq = mf.getTrack(t);
        bool hasNotes = false;
        for (int i = 0; i < seq->getNumEvents(); ++i)
        {
            const auto& msg = seq->getEventPointer(i)->message;
            if (msg.isTrackNameEvent())
                trackNames[(size_t) t] = msg.getTextFromTextMetaEvent().trim();
            else if (msg.isNoteOn() && msg.getVelocity() > 0)
                hasNotes = true;
        }
        if (hasNotes)
            noteTracks.push_back(t);
    }
    if (noteTracks.empty())
    {
        d.error = "No notes in " + file.getFileName();
        return d;
    }

    // Voice grouping: tracks when the file is multi-track (SMF type 1),
    // MIDI channels otherwise (SMF type 0 keeps everything on one track).
    const bool byTrack = noteTracks.size() > 1;
    std::vector<int> channelOrder;   // channel-mode keys, in order of appearance
    auto voiceForChannel = [&channelOrder](int ch) -> int
    {
        for (size_t i = 0; i < channelOrder.size(); ++i)
            if (channelOrder[i] == ch)
                return (int) i;
        channelOrder.push_back(ch);
        return (int) channelOrder.size() - 1;
    };

    // ── Pass 2: extract matched note pairs ───────────────────────────────────
    int mergedNotes = 0, danglingNotes = 0;
    for (size_t ti = 0; ti < noteTracks.size(); ++ti)
    {
        juce::MidiMessageSequence seq(*mf.getTrack(noteTracks[ti]));   // own copy
        seq.updateMatchedPairs();
        const double trackEnd = seq.getEndTime();

        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto* ev  = seq.getEventPointer(i);
            const auto& msg = ev->message;
            if (! msg.isNoteOn() || msg.getVelocity() == 0)
                continue;

            int voice = byTrack ? (int) ti : voiceForChannel(msg.getChannel());
            if (voice >= kMaxVoices)
            {
                voice = kMaxVoices - 1;   // overflow folds into the last voice
                ++mergedNotes;
            }

            NoteEvent n;
            n.note     = msg.getNoteNumber();
            n.velocity = (int) msg.getVelocity();
            n.startSec = msg.getTimeStamp();
            if (ev->noteOffObject != nullptr)
                n.endSec = ev->noteOffObject->message.getTimeStamp();
            else
            {
                n.endSec = trackEnd;      // dangling note-on: close at track end
                ++danglingNotes;
            }
            if (n.endSec <= n.startSec)
                n.endSec = n.startSec + 0.05;   // zero-length guard (drum hits)
            n.voice = voice;

            d.notes.push_back(n);
            d.durationSec = juce::jmax(d.durationSec, n.endSec);
            d.voiceNoteCount[(size_t) voice]++;
        }
    }

    std::sort(d.notes.begin(), d.notes.end(),
              [](const NoteEvent& a, const NoteEvent& b)
              { return a.startSec < b.startSec; });

    // ── Voice names ──────────────────────────────────────────────────────────
    if (byTrack)
    {
        d.numVoices = juce::jmin((int) noteTracks.size(), kMaxVoices);
        for (int v = 0; v < d.numVoices; ++v)
        {
            const int t = noteTracks[(size_t) v];
            const juce::String meta = trackNames[(size_t) t];
            d.voiceNames[(size_t) v] = meta.isNotEmpty()
                ? meta : ("Track " + juce::String(t + 1));
        }
        if ((int) noteTracks.size() > kMaxVoices)
            d.voiceNames[(size_t) (kMaxVoices - 1)]
                += " (+" + juce::String((int) noteTracks.size() - kMaxVoices) + ")";
    }
    else
    {
        d.numVoices = juce::jmin((int) channelOrder.size(), kMaxVoices);
        for (int v = 0; v < d.numVoices; ++v)
            d.voiceNames[(size_t) v] = "Ch " + juce::String(channelOrder[(size_t) v]);
        if ((int) channelOrder.size() > kMaxVoices)
            d.voiceNames[(size_t) (kMaxVoices - 1)]
                += " (+" + juce::String((int) channelOrder.size() - kMaxVoices) + ")";
    }

    // ── Summary ──────────────────────────────────────────────────────────────
    juce::StringArray lines;
    lines.add(file.getFileName() + ": " + juce::String((int) d.notes.size())
              + " notes, " + juce::String(d.numVoices)
              + (byTrack ? " track voice(s), " : " channel voice(s), ")
              + juce::String(d.durationSec, 1) + " s");
    if (mergedNotes > 0)
        lines.add("Voices beyond " + juce::String(kMaxVoices) + " merged into voice "
                  + juce::String(kMaxVoices) + " (" + juce::String(mergedNotes) + " notes)");
    if (danglingNotes > 0)
        lines.add(juce::String(danglingNotes) + " unterminated note(s) closed at track end");
    d.log = lines.joinIntoString("\n");
    d.ok  = true;
    return d;
}

//==============================================================================
// Page maths
//==============================================================================
namespace
{
    inline double mmToPx(double mm, double dpi) { return mm * dpi / 25.4; }

    // SCORE's left label margin is 150 px at 400 DPI — a physical constant.
    constexpr double kLabelMarginMM = 150.0 * 25.4 / 400.0;   // 9.525 mm
}

double pageSeconds(const MidiScoreSettings& s)
{
    if (s.writingSpeed <= 0.0)
        return 0.0;
    const double sheetWidthMM = (s.pageFormat == 1) ? SCORE_A3_WIDTH_MM
                                                    : SCORE_A4_WIDTH_MM;
    const double bandWidthMM = sheetWidthMM - kLabelMarginMM;
    return (bandWidthMM / 10.0) / s.writingSpeed;             // mm→cm, cm / (cm/s)
}

int pageCount(const MidiScoreData& data, const MidiScoreSettings& s)
{
    if (! data.ok || data.notes.empty())
        return 0;
    if (s.pageFormat == 2)                    // FULL: whole piece on one sheet
        return 1;
    const double pageSec = pageSeconds(s);
    if (pageSec <= 0.0)
        return 0;
    return juce::jmax(1, (int) std::ceil(data.durationSec / pageSec));
}

//==============================================================================
// Pan automation
//==============================================================================
double panAt(const std::vector<PanPoint>& points, double posFrac)
{
    if (points.empty())
        return 0.0;
    if (posFrac <= points.front().pos)
        return points.front().pan;
    if (posFrac >= points.back().pos)
        return points.back().pan;
    for (size_t i = 1; i < points.size(); ++i)
    {
        if (posFrac > points[i].pos)
            continue;
        const auto& a = points[i - 1];
        const auto& b = points[i];
        const double span = b.pos - a.pos;
        const double t = span > 1.0e-12 ? (posFrac - a.pos) / span : 1.0;
        const double s = t * t * (3.0 - 2.0 * t);   // S-curve, flat at the handles
        return a.pan + s * (b.pan - a.pan);
    }
    return points.back().pan;
}

//==============================================================================
// Shared drawing core
//==============================================================================
namespace
{
    bool panActive(const std::vector<PanPoint>& points)
    {
        for (const auto& q : points)
            if (std::abs(q.pan) > 0.001)
                return true;
        return false;
    }

    bool anyPanActive(const std::array<std::vector<PanPoint>, kMaxVoices>& all)
    {
        for (const auto& pts : all)
            if (panActive(pts))
                return true;
        return false;
    }
    /** Per-voice partial set, precomputed once per render: the partial RATIOS
     *  (and relative dBs / decay multipliers) are note-independent, so they are
     *  computed at a reference fundamental and rescaled per note. */
    struct VoicePartials
    {
        bool   enabled  = false;
        double refHz    = 440.0;
        double maxAmpDb = -1.0e9;
        double attackSec = 0.004;
        double decaySec  = 0.0;
        double levelDb   = 0.0;
        double vibCents  = 0.0;    // vibrato (see drawNoteVibrato)
        double vibRateHz = 5.5;
        double vibOnsetSec = 0.4;
        double vibLife   = 0.5;
        std::vector<timbregen::Partial> rel;
    };

    std::array<VoicePartials, kMaxVoices> buildVoicePartials(
        const std::array<timbregen::TimbreSlotParams, kMaxVoices>& voices)
    {
        std::array<VoicePartials, kMaxVoices> out;
        for (int v = 0; v < kMaxVoices; ++v)
        {
            const auto& p = voices[(size_t) v];
            auto& vp = out[(size_t) v];
            vp.enabled = p.enabled;
            if (! p.enabled)
                continue;
            timbregen::TimbreSlotParams ref = p;
            ref.midiNote = 69;                       // A4 reference — ratios only
            vp.rel   = timbregen::computePartials(ref);
            vp.refHz = timbregen::midiNoteHz(69);
            for (const auto& pt : vp.rel)
                vp.maxAmpDb = juce::jmax(vp.maxAmpDb, pt.ampDb);
            vp.attackSec = juce::jmax(1.0e-4, p.attackMs / 1000.0);
            vp.decaySec  = p.decaySec;
            vp.levelDb   = p.levelDb;
            vp.vibCents    = juce::jmax(0.0, p.vibCents);
            vp.vibRateHz   = juce::jlimit(0.1, 20.0, p.vibRateHz);
            vp.vibOnsetSec = juce::jmax(0.0, p.vibOnsetSec);
            vp.vibLife     = juce::jlimit(0.0, 1.0, p.vibLife);
        }
        return out;
    }

    inline double velocityDb(int vel, double rangeDb)
    {
        return -rangeDb * (1.0 - (double) juce::jlimit(1, 127, vel) / 127.0);
    }

    /** Loudest printable cell across the WHOLE piece (velocity + strongest
     *  partial; envelopes peak at 0 dB). Page-independent so every export page
     *  shares one ink scale, exactly like SCORE's global_max. The per-voice
     *  Level (dB) is deliberately EXCLUDED from the max: it acts as an
     *  absolute ink/level gain (clipped at full black), instead of being
     *  cancelled by the normalisation whenever only one voice plays.
     *  Returns -1e9 when no enabled voice has notes. */
    double globalMaxDb(const MidiScoreData& data,
                       const std::array<VoicePartials, kMaxVoices>& vps,
                       const MidiScoreSettings& s)
    {
        double mx = -1.0e9;
        for (const auto& n : data.notes)
        {
            const auto& vp = vps[(size_t) n.voice];
            if (! vp.enabled || vp.rel.empty())
                continue;
            mx = juce::jmax(mx, vp.maxAmpDb
                                + velocityDb(n.velocity, s.velocityRangeDb));
        }
        return mx;
    }

    /** Geometry of the band inside the target image + the time window. */
    struct BandGeom
    {
        double x0Px     = 0.0;   ///< px column of t0 (may be fractional)
        int    xMin = 0, xMax = 0;
        double yBottom  = 0.0;   ///< px row of minFreq (band bottom)
        double heightPx = 0.0;
        int    yTop = 0, yBot = 0;
        double pxPerSec = 1.0;
        double t0 = 0.0, t1 = 0.0;
    };

    /** Living vibrato of ONE note, sampled per pixel column.
     *
     *  The whole partial stack is frequency-modulated by the same ±cents wave;
     *  on the LOG axis that is a single vertical offset shared by every
     *  partial, so it is precomputed once per note into two per-column arrays
     *  (pitch offset in px, coupled amplitude dip in dB ≤ 0).
     *
     *  "Living" ingredients — all deterministic in tau (a note crossing a page
     *  boundary keeps its wave phase, and playback matches the print):
     *   - onset: nothing at the attack, then the depth develops smoothly over
     *     vibOnsetSec (delay + smoothstep rise), and eases out at the tail;
     *   - waving depth: a slow undulation makes the vibrato come and go
     *     instead of holding a constant amplitude;
     *   - drifting rate: the rate wobbles a few percent (integrated
     *     analytically so the phase stays a pure function of tau);
     *   - per-note defects: each note draws its own start phase, rate and
     *     depth deviation from a hash of its index — two equal notes never
     *     vibrate identically.
     *  vibLife scales every irregularity: 0 = metronomic sine, 1 = loose. */
    void computeNoteVibrato(const VoicePartials& vp, const NoteEvent& n,
                            size_t noteIndex, const BandGeom& g,
                            int x0, int x1, double pxPerCent,
                            std::vector<double>& yOffPx,
                            std::vector<double>& amDb)
    {
        const double life = vp.vibLife;

        // Deterministic per-note randoms (no global RNG — pure re-render).
        juce::uint32 h = (juce::uint32) (noteIndex + 1) * 2654435761u
                       ^ (juce::uint32) (n.note * 40503 + 977);
        auto rnd = [&h]() {
            h = h * 1664525u + 1013904223u;
            return (double) (h >> 8) * (1.0 / 16777216.0);
        };
        const double phase0   = rnd() * juce::MathConstants<double>::twoPi * life;
        const double rateMul  = 1.0 + (rnd() - 0.5) * 0.16 * life;   // ±8 %
        const double depthMul = 1.0 + (rnd() - 0.5) * 0.50 * life;   // ±25 %
        const double driftPh  = rnd() * juce::MathConstants<double>::twoPi;
        const double undPh    = rnd() * juce::MathConstants<double>::twoPi;
        const double undHz    = 0.8 * (0.6 + 0.8 * rnd());           // 0.48..1.12 Hz

        const double dur     = n.endSec - n.startSec;
        const double delay   = vp.vibOnsetSec * 0.4;
        const double rise    = juce::jmax(0.08, vp.vibOnsetSec * 0.6);
        const double easeSec = juce::jmin(0.25, dur * 0.25);         // tail ease-out
        const double rate    = vp.vibRateHz * rateMul;
        const double driftHz = 0.9;                                  // rate-wobble speed
        const double driftW  = juce::MathConstants<double>::twoPi * driftHz;
        const double driftA  = 0.10 * life;                          // ±10 % rate wobble
        const double twoPi   = juce::MathConstants<double>::twoPi;

        const size_t nCols = (size_t) juce::jmax(0, x1 - x0);
        yOffPx.assign(nCols, 0.0);
        amDb.assign(nCols, 0.0);

        for (size_t i = 0; i < nCols; ++i)
        {
            const double tau = (((double) (x0 + (int) i) + 0.5) - g.x0Px) / g.pxPerSec
                             + g.t0 - n.startSec;
            if (tau < 0.0 || tau > dur)
                continue;

            double env = 0.0;                                        // onset smoothstep
            if (tau > delay)
            {
                const double s = juce::jlimit(0.0, 1.0, (tau - delay) / rise);
                env = s * s * (3.0 - 2.0 * s);
            }
            if (easeSec > 0.0)
            {
                const double e = juce::jlimit(0.0, 1.0, (dur - tau) / easeSec);
                env *= e * e * (3.0 - 2.0 * e);
            }
            if (env <= 0.0)
                continue;

            // Depth undulation: comes and goes between 100 % and (1 − 0.35·life).
            const double und = 1.0 - 0.35 * life
                             * (0.5 + 0.5 * std::sin(twoPi * undHz * tau + undPh));

            // Phase with drifting rate, integrated analytically:
            // rate(t) = R(1 + a·sin(wt+p))  →  ∫ = R·tau − (R·a/w)(cos(wtau+p) − cos p)
            const double phase = twoPi * rate * tau
                               + (twoPi * rate * driftA / driftW)
                                 * (std::cos(driftPh) - std::cos(driftW * tau + driftPh))
                               + phase0;

            const double amp = vp.vibCents * depthMul * env * und;   // cents, now
            yOffPx[i] = amp * std::sin(phase) * pxPerCent;

            // Subtle coupled AM (never above the note's own level): a breath
            // of ~0.6 dB per 30 cents, in quadrature with the pitch wave.
            const double amDepth = 0.018 * amp;
            amDb[i] = amDepth * 0.5 * (std::sin(phase - juce::MathConstants<double>::halfPi) - 1.0);
        }
    }

    /** Draws every note overlapping [t0..t1] into the band: one soft-edged
     *  horizontal line per partial, attack/decay envelope along the note,
     *  end fade for anti-click / bar separation, per-voice living vibrato
     *  waving the whole partial stack. Same greyscale-vs-dB and
     *  Gaussian cross-profile conventions as TimbreGenRenderer. */
    void drawNotes(juce::Image& img, const BandGeom& g,
                   const MidiScoreData& data,
                   const std::array<VoicePartials, kMaxVoices>& vps,
                   const MidiScoreSettings& s,
                   double maxDb, double dpiY,
                   const std::function<void(double)>* progress = nullptr)
    {
        const double range     = juce::jmax(1.0, s.dynamicRangeDB);
        const double logRatio  = std::log(s.maxFreq / s.minFreq);
        const double halfWidth = juce::jmax(0.5, mmToPx(s.lineWidthMM, dpiY) * 0.5);
        const int    dyMax     = (int) std::ceil(halfWidth * 2.0);   // Gaussian skirt

        // On the LOG axis a pitch offset in cents is the same px offset at any
        // frequency — one conversion serves the whole vibrato render.
        const double pxPerCent = (std::log(2.0) / 1200.0) / logRatio * g.heightPx;
        std::vector<double> vibYOff, vibAmDb;   // per-column, reused across notes

        const int imageW = img.getWidth();
        juce::Image::BitmapData bmp(img, juce::Image::BitmapData::readWrite);
        auto darken = [&](int x, int y, double intensity)   // 0 = black … 1 = white
        {
            if (x < 0 || x >= imageW || y < g.yTop || y >= g.yBot) return;
            const auto v = (juce::uint8) juce::jlimit(0, 255,
                                (int) (intensity * 255.0 + 0.5));
            juce::uint8* p = bmp.getLinePointer(y) + x * bmp.pixelStride;
            if (v < p[0]) p[0] = p[1] = p[2] = v;            // darker (louder) wins
        };

        // Panned ink, SCORE stereo convention: R byte = RIGHT brightness,
        // B byte = LEFT brightness, G = the darker of the two — left-only ink
        // is red, right-only blue, centre grey (byte-identical to mono).
        // Darker-wins per CHANNEL so overlapping notes with different pans
        // composite exactly like ScoreGen's stereo cells.
        auto darkenLR = [&](int x, int y, double intL, double intR)
        {
            if (x < 0 || x >= imageW || y < g.yTop || y >= g.yBot) return;
            const auto vL = (juce::uint8) juce::jlimit(0, 255,
                                (int) (intL * 255.0 + 0.5));
            const auto vR = (juce::uint8) juce::jlimit(0, 255,
                                (int) (intR * 255.0 + 0.5));
            auto* p = reinterpret_cast<juce::PixelRGB*>(
                bmp.getLinePointer(y) + x * bmp.pixelStride);
            p->setARGB(255,
                       juce::jmin(p->getRed(),   vR),
                       juce::jmin(p->getGreen(), juce::jmin(vL, vR)),
                       juce::jmin(p->getBlue(),  vL));
        };

        // ── Pan automation → per-VOICE, per-column L/R attenuations (dB ≤ 0).
        // Linear balance: centre = 0 dB both sides (grey ink, historical
        // bytes), panning only attenuates the far side; the reader's stereo
        // decode rebalances loudness through GREEN, like SCORE stereo.
        // A voice with no real pan keeps empty arrays (= centre everywhere).
        const bool hasPan = anyPanActive(s.panPoints) && data.durationSec > 0.0;
        std::array<std::vector<double>, kMaxVoices> panDbL, panDbR;
        if (hasPan)
        {
            const size_t nCols = (size_t) juce::jmax(0, g.xMax - g.xMin);
            for (int v = 0; v < kMaxVoices; ++v)
            {
                if (! vps[(size_t) v].enabled
                    || ! panActive(s.panPoints[(size_t) v]))
                    continue;
                auto& dbL = panDbL[(size_t) v];
                auto& dbR = panDbR[(size_t) v];
                dbL.resize(nCols);
                dbR.resize(nCols);
                for (size_t i = 0; i < nCols; ++i)
                {
                    const double t = (((double) (g.xMin + (int) i) + 0.5) - g.x0Px)
                                   / g.pxPerSec + g.t0;
                    const double p = juce::jlimit(-1.0, 1.0,
                        panAt(s.panPoints[(size_t) v],
                              juce::jlimit(0.0, 1.0, t / data.durationSec)));
                    // Gain orientation fixed EMPIRICALLY (2026-07-24): with
                    // the "obvious" assignment (gL = 1−p) the rendered tint
                    // came out mirrored against the UI curve — red curve gave
                    // blue ink. This orientation is the one verified on
                    // screen: top/red handle (p = −1) → red ink → left ear.
                    const double gL = juce::jmin(1.0, 1.0 + p);
                    const double gR = juce::jmin(1.0, 1.0 - p);
                    dbL[i] = gL <= 1.0e-6 ? -1.0e9 : 20.0 * std::log10(gL);
                    dbR[i] = gR <= 1.0e-6 ? -1.0e9 : 20.0 * std::log10(gR);
                }
            }
        }

        for (size_t ni = 0; ni < data.notes.size(); ++ni)
        {
            if (progress != nullptr && (ni & 127u) == 0u)
                (*progress)((double) ni / (double) data.notes.size());

            const auto& n  = data.notes[ni];
            const auto& vp = vps[(size_t) n.voice];
            if (! vp.enabled || vp.rel.empty())
                continue;
            if (n.endSec <= g.t0 || n.startSec >= g.t1)
                continue;   // outside this window

            const double f0      = timbregen::midiNoteHz(n.note);
            const double baseAll = velocityDb(n.velocity, s.velocityRangeDb)
                                 + vp.levelDb - maxDb;                 // ≤ 0
            if (baseAll <= -range)
                continue;

            const double dur     = n.endSec - n.startSec;
            const double fadeSec = juce::jmin(0.04, dur * 0.2);   // anti-click tail

            const double tA = juce::jmax(n.startSec, g.t0);
            const double tB = juce::jmin(n.endSec,   g.t1);
            const int x0 = juce::jmax(g.xMin, (int) std::floor(g.x0Px + (tA - g.t0) * g.pxPerSec));
            const int x1 = juce::jmin(g.xMax, (int) std::ceil (g.x0Px + (tB - g.t0) * g.pxPerSec));

            // Vibrato wave of this note — one array for the whole partial stack.
            const bool hasVib = vp.vibCents > 0.05 && pxPerCent > 0.0 && x1 > x0;
            int vibMarginPx = 0;
            if (hasVib)
            {
                computeNoteVibrato(vp, n, ni, g, x0, x1, pxPerCent, vibYOff, vibAmDb);
                vibMarginPx = (int) std::ceil(vp.vibCents * 1.25 * pxPerCent);
            }

            // This VOICE's pan attenuations (nullptr = centred voice).
            const double* vPanL = panDbL[(size_t) n.voice].empty()
                                      ? nullptr : panDbL[(size_t) n.voice].data();
            const double* vPanR = panDbR[(size_t) n.voice].empty()
                                      ? nullptr : panDbR[(size_t) n.voice].data();

            for (const auto& pt : vp.rel)
            {
                const double f = f0 * (pt.freqHz / vp.refHz);
                if (f < s.minFreq || f > s.maxFreq)
                    continue;   // partial outside the instrument's span
                const double baseDb = baseAll + pt.ampDb;
                if (baseDb <= -range)
                    continue;

                // LOG frequency axis: same mapping as SCORE / TIMBRE / the reader.
                const double pos = std::log(f / s.minFreq) / logRatio;
                const double yC  = g.yBottom - pos * g.heightPx;
                const int    yCi = (int) std::round(yC);
                if (yCi + dyMax + vibMarginPx < g.yTop
                    || yCi - dyMax - vibMarginPx >= g.yBot)
                    continue;

                for (int x = x0; x < x1; ++x)
                {
                    // Column time relative to the NOTE start (absolute — so a
                    // note crossing a page boundary keeps its envelope phase).
                    const double tau = ((x + 0.5) - g.x0Px) / g.pxPerSec
                                     + g.t0 - n.startSec;
                    if (tau < 0.0 || tau > dur)
                        continue;

                    double envDb = 0.0;
                    if (tau < vp.attackSec)                          // attack ramp
                        envDb += 20.0 * std::log10(juce::jmax(tau / vp.attackSec, 1.0e-4));
                    if (vp.decaySec > 0.0 && tau > vp.attackSec)     // −60 dB decay
                        envDb += -60.0 * (tau - vp.attackSec) / vp.decaySec * pt.decayMul;
                    const double tail = dur - tau;
                    if (tail < fadeSec)
                        envDb += 20.0 * std::log10(juce::jmax(tail / fadeSec, 1.0e-4));

                    double yCol = yC;
                    if (hasVib)
                    {
                        yCol -= vibYOff[(size_t) (x - x0)];          // +cents = higher = up
                        envDb += vibAmDb[(size_t) (x - x0)];
                    }

                    const double dB = baseDb + envDb;
                    if (dB <= -range)
                        continue;

                    const double pdL = vPanL ? vPanL[x - g.xMin] : 0.0;
                    const double pdR = vPanR ? vPanR[x - g.xMin] : 0.0;

                    const int yCiCol = (int) std::round(yCol);
                    for (int dy = -dyMax; dy <= dyMax; ++dy)
                    {
                        const double dd  = ((yCiCol + dy) - yCol) / halfWidth;
                        const double off = -12.0 * dd * dd;          // Gaussian in dB
                        if (! hasPan)
                        {
                            const double v = juce::jlimit(0.0, 1.0, -(dB + off) / range);
                            if (v < 1.0)
                                darken(x, yCiCol + dy, v);
                        }
                        else
                        {
                            const double vL = juce::jlimit(0.0, 1.0,
                                                  -(dB + off + pdL) / range);
                            const double vR = juce::jlimit(0.0, 1.0,
                                                  -(dB + off + pdR) / range);
                            if (vL < 1.0 || vR < 1.0)
                                darkenLR(x, yCiCol + dy, vL, vR);
                        }
                    }
                }
            }
        }
    }
}

//==============================================================================
// Strip render (playback frames / UI preview)
//==============================================================================
scoregen::RenderResult renderStrip(
    const MidiScoreData& data,
    const std::array<timbregen::TimbreSlotParams, kMaxVoices>& voices,
    const MidiScoreSettings& settings,
    double t0Sec, double t1Sec,
    double pxPerSec, double dpiY)
{
    scoregen::RenderResult result;
    auto fail = [&](const juce::String& msg) -> scoregen::RenderResult
    {
        result.ok  = false;
        result.log = msg;
        return result;
    };

    if (! data.ok || data.notes.empty())
        return fail(data.error.isNotEmpty() ? data.error : juce::String("No MIDI file loaded"));
    if (settings.minFreq <= 0.0 || settings.maxFreq <= settings.minFreq)
        return fail("Invalid frequency range");
    if (t1Sec <= t0Sec || pxPerSec <= 0.0 || dpiY < 8.0)
        return fail("Degenerate strip geometry");

    const auto vps   = buildVoicePartials(voices);
    const double mx  = globalMaxDb(data, vps, settings);
    if (mx < -1.0e8)
        return fail("No voice enabled (activate at least one voice)");

    const int w = juce::jmax(1, (int) std::ceil((t1Sec - t0Sec) * pxPerSec));
    const int h = juce::jmax(2, (int) std::round(mmToPx(settings.spectroHeightMM, dpiY)));
    if ((juce::int64) w * (juce::int64) h > (juce::int64) 200'000'000)
        return fail("Strip too large (" + juce::String(w) + " x " + juce::String(h)
                    + juce::String::fromUTF8(" px) — lower the resolution or shorten the window"));

    juce::Image img(juce::Image::RGB, w, h, true);
    {
        juce::Graphics g(img);
        g.fillAll(juce::Colours::white);
    }

    BandGeom geom;
    geom.x0Px = 0.0;  geom.xMin = 0;  geom.xMax = w;
    geom.yBottom = (double) h;  geom.heightPx = (double) h;
    geom.yTop = 0;  geom.yBot = h;
    geom.pxPerSec = pxPerSec;  geom.t0 = t0Sec;  geom.t1 = t1Sec;
    drawNotes(img, geom, data, vps, settings, mx, dpiY);

    result.image       = img;
    result.ok          = true;
    result.stereo      = anyPanActive(settings.panPoints); // colour ink ⇒ stereo load
    result.pixelWidth  = w;
    result.pixelHeight = h;
    result.spectroBand = img.getBounds();
    result.log = "Strip: " + juce::String(w) + " x " + juce::String(h) + " px, "
               + juce::String(t1Sec - t0Sec, 1) + " s @ "
               + juce::String(pxPerSec, 0) + " px/s";
    return result;
}

//==============================================================================
// Export sheet (A4 / A3 / FULL) — free start time
//==============================================================================
scoregen::RenderResult renderSheet(
    const MidiScoreData& data,
    const std::array<timbregen::TimbreSlotParams, kMaxVoices>& voices,
    const MidiScoreSettings& settings,
    double t0Sec,
    const juce::String& pageTag,
    const std::function<void(double)>& progress)
{
    scoregen::RenderResult result;
    auto fail = [&](const juce::String& msg) -> scoregen::RenderResult
    {
        result.ok  = false;
        result.log = msg;
        return result;
    };

    if (! data.ok || data.notes.empty())
        return fail(data.error.isNotEmpty() ? data.error : juce::String("No MIDI file loaded"));
    if (settings.minFreq <= 0.0 || settings.maxFreq <= settings.minFreq)
        return fail("Invalid frequency range");
    if (settings.writingSpeed <= 0.0)
        return fail("Writing speed must be > 0");

    const double dpi = (settings.printerDpi >= 72.0) ? settings.printerDpi
                                                     : SCORE_DEFAULT_PRINTER_DPI;

    const auto vps  = buildVoicePartials(voices);
    const double mx = globalMaxDb(data, vps, settings);
    if (mx < -1.0e8)
        return fail("No voice enabled (activate at least one voice)");

    // ── Sheet geometry (same band placement as SCORE/TIMBRE; A4 portrait and
    //    A3 landscape share the 297 mm height, FULL stretches the width to
    //    hold the whole piece on one sheet) ─────────────────────────────────
    const bool   full        = settings.pageFormat == 2;
    const double pxPerSec    = (dpi / 2.54) * settings.writingSpeed;
    const double labelMargin = 150.0 * (dpi / 400.0);            // SCORE's left margin
    double windowSec;
    int    imageW;
    if (full)
    {
        t0Sec     = 0.0;
        windowSec = juce::jmax(0.05, data.durationSec);
        imageW    = (int) std::ceil(labelMargin + windowSec * pxPerSec);
    }
    else
    {
        t0Sec     = juce::jmax(0.0, t0Sec);
        windowSec = pageSeconds(settings);
        imageW    = (int) mmToPx(settings.pageFormat == 1 ? SCORE_A3_WIDTH_MM
                                                          : SCORE_A4_WIDTH_MM, dpi);
    }
    const int imageH = (int) mmToPx(SCORE_A4_HEIGHT_MM, dpi);
    // Only FULL can trip this (A3@800 DPI peaks at ~124 Mpx): its width grows
    // with duration × writing speed × DPI. SLOWER writing = shorter sheet.
    // 500 Mpx ≈ 1.5 GB transient RGB — enough for minutes-long pieces at
    // 400 DPI; beyond that the render/encode would genuinely bog down.
    if ((juce::int64) imageW * (juce::int64) imageH > (juce::int64) 500'000'000)
        return fail("Sheet too large (" + juce::String(imageW) + " x "
                    + juce::String(imageH)
                    + juce::String::fromUTF8(" px) — FULL holds the whole piece:"
                                             " lower the DPI or the writing speed"));

    const double bottomMarginPx  = mmToPx(settings.bottomMarginMM,  dpi);
    const double spectroHeightPx = mmToPx(settings.spectroHeightMM, dpi);
    const double spectroLeft     = labelMargin;
    const double spectroBottom   = imageH - bottomMarginPx;
    const double spectroTop      = spectroBottom - spectroHeightPx;
    const double spectroWidth    = imageW - labelMargin;
    if (spectroTop < 0.0 || spectroWidth <= 0.0)
        return fail("Band does not fit the page at these margins");

    const double t0 = t0Sec;

    juce::Image img(juce::Image::RGB, imageW, imageH, true);
    {
        juce::Graphics g(img);
        g.fillAll(juce::Colours::white);
    }

    const int yTop = juce::jmax(0,      (int) std::floor(spectroTop));
    const int yBot = juce::jmin(imageH, (int) std::ceil (spectroBottom));

    BandGeom geom;
    geom.x0Px = spectroLeft;
    geom.xMin = (int) std::floor(spectroLeft);
    geom.xMax = juce::jmin(imageW, (int) std::ceil(spectroLeft + spectroWidth));
    geom.yBottom = spectroBottom;  geom.heightPx = spectroHeightPx;
    geom.yTop = yTop;  geom.yBot = yBot;
    geom.pxPerSec = pxPerSec;  geom.t0 = t0;  geom.t1 = t0 + windowSec;
    drawNotes(img, geom, data, vps, settings, mx, dpi,
              progress ? &progress : nullptr);
    if (progress)
        progress(1.0);

    // ── Opt-in writings in the TOP margin (far from the scanned band) ─────────
    if (settings.showLabels)
    {
        juce::Graphics g(img);
        const float labelH = (float) mmToPx(4.0, dpi);
        g.setColour(juce::Colours::black);
        g.setFont(juce::FontOptions(labelH * 0.72f));
        g.drawText(juce::File(data.sourcePath).getFileName()
                       + (pageTag.isNotEmpty()
                              ? juce::String::fromUTF8("  —  ") + pageTag
                              : juce::String()),
                   juce::Rectangle<float>((float) spectroLeft,
                                          (float) mmToPx(2.0, dpi),
                                          (float) spectroWidth, labelH),
                   juce::Justification::centredLeft);

        // Footer: the settings needed to reproduce / play the print in tune.
        g.setColour(juce::Colour(0xff707070));
        g.setFont(juce::FontOptions((float) mmToPx(2.6, dpi)));
        g.drawText("Sp3ctra MIDI SCORE  |  " + juce::String(settings.minFreq, 0) + "-"
                       + juce::String(settings.maxFreq, 0) + " Hz log  |  band "
                       + juce::String(settings.spectroHeightMM, 3) + " mm  |  "
                       + juce::String(settings.writingSpeed, 1) + " cm/s  |  "
                       + juce::String(t0, 1) + "-" + juce::String(t0 + windowSec, 1) + " s  |  "
                       + juce::String(dpi, 0) + juce::String::fromUTF8(" DPI — print at 100%"),
                   juce::Rectangle<float>((float) spectroLeft,
                                          (float) mmToPx(6.5, dpi),
                                          (float) spectroWidth, (float) mmToPx(3.5, dpi)),
                   juce::Justification::centredLeft);
    }

    result.image       = img;
    result.ok          = true;
    result.stereo      = anyPanActive(settings.panPoints); // colour print (L=red/R=blue)
    result.pixelWidth  = imageW;
    result.pixelHeight = imageH;
    result.spectroBand = juce::Rectangle<int>(
        (int) std::floor(spectroLeft), yTop,
        juce::jmax(1, geom.xMax - (int) std::floor(spectroLeft)),
        juce::jmax(1, yBot - yTop));
    result.log = "Sheet " + juce::String(imageW) + " x " + juce::String(imageH)
               + " px @ " + juce::String(dpi, 0) + " DPI, "
               + juce::String(t0, 1) + "-" + juce::String(t0 + windowSec, 1) + " s"
               + (pageTag.isNotEmpty() ? " (" + pageTag + ")" : juce::String());
    return result;
}

scoregen::RenderResult renderPage(
    const MidiScoreData& data,
    const std::array<timbregen::TimbreSlotParams, kMaxVoices>& voices,
    const MidiScoreSettings& settings,
    int pageIndex,
    const std::function<void(double)>& progress)
{
    const int nPages = pageCount(data, settings);
    if (nPages > 0 && (pageIndex < 0 || pageIndex >= nPages))
    {
        scoregen::RenderResult r;
        r.ok  = false;
        r.log = "Page index out of range";
        return r;
    }
    return renderSheet(data, voices, settings,
                       pageIndex * pageSeconds(settings),
                       nPages > 1 ? ("page " + juce::String(pageIndex + 1)
                                     + "/" + juce::String(nPages))
                                  : juce::String(),
                       progress);
}

} // namespace midiscoregen
