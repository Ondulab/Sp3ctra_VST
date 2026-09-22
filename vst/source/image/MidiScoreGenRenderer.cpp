#include "MidiScoreGenRenderer.h"

#include <cmath>

namespace midiscoregen
{

//==============================================================================
// MIDI parsing
//==============================================================================
namespace
{
    /** Slice the [t0..t1] window of a per-channel controller curve into
     *  note-relative breakpoints, seeding the value in force at t0. Leaves
     *  `dst` empty when the curve never departs from `def` over the note. */
    void attachCurve(const std::vector<std::pair<double, float>>& src,
                     double t0, double t1, float def,
                     std::vector<std::pair<float, float>>& dst)
    {
        float cur = def;
        for (const auto& p : src)
        {
            if (p.first <= t0) { cur = p.second; continue; }
            if (p.first >= t1) break;
            if (dst.empty())
                dst.push_back({ 0.0f, cur });
            dst.push_back({ (float) (p.first - t0), p.second });
        }
        if (dst.empty() && cur != def)
            dst.push_back({ 0.0f, cur });   // constant but non-default
    }
}

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

    // Sp3ctra MIDI TAP takes carry their origin as the track name. An MPE
    // take spreads ONE musical voice over 15 channels (pure allocation), so
    // captures fold every channel into voice 0.
    for (const auto& name : trackNames)
        if (name.startsWith("Sp3ctra MIDI TAP"))
            d.sp3ctraCapture = true;

    // Voice grouping: tracks when the file is multi-track (SMF type 1),
    // MIDI channels otherwise (SMF type 0 keeps everything on one track).
    const bool byTrack = ! d.sp3ctraCapture && noteTracks.size() > 1;
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

        // Per-channel controller curves (MPE takes: bend = the crest's real
        // trajectory, pressure/CC11 = the level envelope). Bend range read as
        // the MIDI default ±2 st — the writer's own RPN convention.
        std::array<std::vector<std::pair<double, float>>, 16> chBend, chLvl;
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto& m = seq.getEventPointer(i)->message;
            const int   c = m.getChannel() - 1;
            if (c < 0 || c > 15)
                continue;
            if (m.isPitchWheel())
                chBend[(size_t) c].push_back({ m.getTimeStamp(),
                    (float) ((m.getPitchWheelValue() - 8192) * (200.0 / 8192.0)) });
            else if (m.isChannelPressure())
                chLvl[(size_t) c].push_back({ m.getTimeStamp(),
                    (float) m.getChannelPressureValue() });
            else if (m.isController() && m.getControllerNumber() == 11)
                chLvl[(size_t) c].push_back({ m.getTimeStamp(),
                    (float) m.getControllerValue() });
        }

        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto* ev  = seq.getEventPointer(i);
            const auto& msg = ev->message;
            if (! msg.isNoteOn() || msg.getVelocity() == 0)
                continue;

            int voice = d.sp3ctraCapture ? 0
                      : byTrack ? (int) ti : voiceForChannel(msg.getChannel());
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

            const size_t c = (size_t) juce::jlimit(0, 15, msg.getChannel() - 1);
            if (! chBend[c].empty())
                attachCurve(chBend[c], n.startSec, n.endSec, 0.0f, n.bendPts);
            if (! chLvl[c].empty())
                attachCurve(chLvl[c], n.startSec, n.endSec,
                            (float) n.velocity, n.levelPts);

            d.notes.push_back(n);
            d.durationSec = juce::jmax(d.durationSec, n.endSec);
            d.voiceNoteCount[(size_t) voice]++;
        }
    }

    std::sort(d.notes.begin(), d.notes.end(),
              [](const NoteEvent& a, const NoteEvent& b)
              { return a.startSec < b.startSec; });

    // ── Voice names ──────────────────────────────────────────────────────────
    if (d.sp3ctraCapture)
    {
        d.numVoices = 1;
        d.voiceNames[0] = "Capture";
    }
    else if (byTrack)
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
    if (d.sp3ctraCapture)
        lines.add("Sp3ctra capture: channels folded into one voice, "
                  "timbre set to Sine (each note is already a partial)");
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
// Shared drawing core — every note goes through timbregen::NotePainter, the
// painter TIMBRE uses for its strips, so a timbre sounds the same under a
// piece as on its own page. What stays here is what is MIDI SCORE's: the
// voices, the per-voice pan automation and EQ, the piece-wide ink scale.
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

    using VoiceModels = std::array<timbregen::VoiceModel, kMaxVoices>;

    VoiceModels buildVoiceModels(
        const std::array<timbregen::TimbreSlotParams, kMaxVoices>& voices)
    {
        VoiceModels out;
        for (int v = 0; v < kMaxVoices; ++v)
            out[(size_t) v] = timbregen::buildVoiceModel(voices[(size_t) v]);
        return out;
    }

    /** Loudest printable cell across the WHOLE piece (velocity + strongest
     *  partial; envelopes peak at 0 dB). Page-independent so every export page
     *  shares one ink scale, exactly like SCORE's global_max. The per-voice
     *  Level (dB) is deliberately EXCLUDED from the max: it acts as an
     *  absolute ink/level gain (clipped at full black), instead of being
     *  cancelled by the normalisation whenever only one voice plays.
     *  Returns -1e9 when no enabled voice has notes. */
    double globalMaxDb(const MidiScoreData& data, const VoiceModels& vms,
                       const MidiScoreSettings& s)
    {
        double mx = -1.0e9;
        for (const auto& n : data.notes)
        {
            const auto& vm = vms[(size_t) n.voice];
            if (! vm.hasInk())
                continue;
            mx = juce::jmax(mx, vm.maxAmpDb
                                + timbregen::velocityDb(n.velocity, s.velocityRangeDb));
        }
        return mx;
    }

    /** Draws every note overlapping [t0..t1] into the band through the
     *  shared painter: partial stack, envelope, vibrato/drift, captured
     *  curves, textures — plus this page's per-voice pan tint and EQ. */
    void drawNotes(juce::Image& img, const timbregen::BandGeom& g,
                   const MidiScoreData& data,
                   const VoiceModels& vms,
                   const MidiScoreSettings& s,
                   double maxDb, double dpiY,
                   const std::function<void(double)>* progress = nullptr)
    {
        timbregen::InkSettings ink;
        ink.minFreq        = s.minFreq;
        ink.maxFreq        = s.maxFreq;
        ink.dynamicRangeDB = s.dynamicRangeDB;
        ink.lineWidthMM    = s.lineWidthMM;
        ink.dpiY           = dpiY;
        ink.maxDb          = maxDb;
        timbregen::NotePainter painter(img, g, ink);

        // Per-voice EQ: the curve is a tone control of the VOICE, so its gain
        // is read at each PARTIAL's frequency (not at an image row) — hoisted
        // here so only voices that actually shape pay for it.
        std::array<std::function<double(double)>, kMaxVoices> eqFn;
        std::array<bool, kMaxVoices> eqOn {};
        for (int v = 0; v < kMaxVoices; ++v)
        {
            eqOn[(size_t) v] = vms[(size_t) v].enabled && s.voiceEq[(size_t) v].active();
            if (eqOn[(size_t) v])
            {
                const auto* eq = &s.voiceEq[(size_t) v];
                eqFn[(size_t) v] = [eq](double hz) { return (double) eq->gainDbAt(hz); };
            }
        }

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
                if (! vms[(size_t) v].enabled
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
            const auto& vm = vms[(size_t) n.voice];
            if (! vm.hasInk())
                continue;

            timbregen::NoteInk ink1;
            ink1.f0Hz      = timbregen::midiNoteHz(n.note);
            ink1.startSec  = n.startSec;
            ink1.endSec    = n.endSec;
            ink1.levelDb   = timbregen::velocityDb(n.velocity, s.velocityRangeDb) + vm.levelDb;
            ink1.noteIndex = ni;
            ink1.bendPts   = n.bendPts.empty()  ? nullptr : &n.bendPts;
            ink1.levelPts  = n.levelPts.empty() ? nullptr : &n.levelPts;
            ink1.velocity  = n.velocity;
            ink1.velocityRangeDb = s.velocityRangeDb;
            if (! panDbL[(size_t) n.voice].empty())
            {
                ink1.panDbL = panDbL[(size_t) n.voice].data();
                ink1.panDbR = panDbR[(size_t) n.voice].data();
            }
            if (eqOn[(size_t) n.voice])
                ink1.gainDbAt = &eqFn[(size_t) n.voice];

            painter.paint(vm, ink1);
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

    const auto vps   = buildVoiceModels(voices);
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

    timbregen::BandGeom geom;
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

    const auto vps  = buildVoiceModels(voices);
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

    timbregen::BandGeom geom;
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
