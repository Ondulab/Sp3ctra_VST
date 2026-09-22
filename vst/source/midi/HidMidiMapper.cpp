/**
 * @file HidMidiMapper.cpp
 * @brief The CIS as a MIDI controller — see header.
 */
#include "HidMidiMapper.h"
#include <cmath>

extern "C" {
#include "slp_rx_state.h"
}

namespace
{
    struct ControlDef { const char* key; const char* name; const char* unit; float lo, hi, defMin, defMax; int defNum; };

    const ControlDef kDefs[HidMidiMapper::NumControls] = {
        { "Sw1",   "SW1",    "",    0, 0,       0, 0,     20 },
        { "Sw2",   "SW2",    "",    0, 0,       0, 0,     21 },
        { "Sw3",   "SW3",    "",    0, 0,       0, 0,     22 },
        { "GyrX",  "GYRO X", "dps", -2000.f, 2000.f, -250.f,  250.f,  33 },
        { "GyrY",  "GYRO Y", "dps", -2000.f, 2000.f, -250.f,  250.f,  34 },
        { "GyrZ",  "GYRO Z", "dps", -2000.f, 2000.f, -250.f,  250.f,  35 },
        { "TiltP", "TILT P", "deg", -90.f,   90.f,   -45.f,   45.f,   40 },
        { "TiltR", "TILT R", "deg", -90.f,   90.f,   -45.f,   45.f,   41 },
        // device-side gestures: the HIT family behaves like buttons (one pulse
        // per event), FACE is a selector of the resting face — mapped by the
        // per-face value table, so its lo/hi/defMin/defMax are unused. HIT
        // fires on every shock; the four others only on the face that took
        // it. Their default CC numbers keep FACE on 53 so mappings learnt
        // before the per-face rows existed survive.
        { "Hit",      "HIT",       "",    0, 0,       0, 0,     50 },
        { "HitPaper", "HIT PAPER", "",    0, 0,       0, 0,     51 },
        { "HitBack",  "HIT BACK",  "",    0, 0,       0, 0,     52 },
        { "HitLeft",  "HIT LEFT",  "",    0, 0,       0, 0,     54 },
        { "HitRight", "HIT RIGHT", "",    0, 0,       0, 0,     55 },
        { "Face",     "FACE",      "",    0.f, 4.f,   0.f, 4.f, 53 },
    };

    // enum slp_face order. Table defaults spread the faces evenly over the
    // course, i.e. exactly the linear law the table replaced.
    const char* kFaceNames[HidMidiMapper::kNumFaces] =
        { "Moving", "Paper", "Back", "Left", "Right" };

    constexpr float kHidPeriodMs = 5.0f;      // 200 Hz default rate
    constexpr float kPulseMs     = 60.0f;     // trigger press -> release delay
    constexpr float kVelPulseMs  = 15.0f;     // Velocity: hug the impulse, no more
    constexpr float kMeterHoldMs = 60.0f;     // ... but stay visible on the page
    constexpr float kRestGateDps = 3.0f;      // gyro magnitude below = still
    constexpr float kRestGateMs  = 400.0f;    // still this long -> gate closes
}

const char* HidMidiMapper::controlKey  (int c) noexcept { return kDefs[c].key; }
const char* HidMidiMapper::controlName (int c) noexcept { return kDefs[c].name; }
const char* HidMidiMapper::controlUnit (int c) noexcept { return kDefs[c].unit; }
juce::String HidMidiMapper::paramId (int c, const char* suffix) { return juce::String (kPrefix) + kDefs[c].key + suffix; }
const char* HidMidiMapper::faceName (int f) noexcept { return kFaceNames[f]; }
juce::String HidMidiMapper::faceParamId (int f) { return juce::String (kPrefix) + "FaceVal" + juce::String (f); }

//==============================================================================
void HidMidiMapper::addParameters (std::vector<std::unique_ptr<juce::RangedAudioParameter>>& params)
{
    const auto hiddenChoice = juce::AudioParameterChoiceAttributes{}.withAutomatable (false);
    const auto hiddenInt    = juce::AudioParameterIntAttributes{}.withAutomatable (false);
    const auto hiddenFloat  = juce::AudioParameterFloatAttributes{}.withAutomatable (false);
    const auto hiddenBool   = juce::AudioParameterBoolAttributes{}.withAutomatable (false);

    for (int c = 0; c < NumControls; ++c)
    {
        const auto& d = kDefs[c];
        const juce::String name = juce::String ("CIS ") + d.name;
        if (isEvent (c))
            // Velocity by default: a percussion trigger whose CC ignored how
            // hard you hit was the whole complaint.
            params.push_back (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { paramId (c, "Type"), 1 }, name + " Type",
                juce::StringArray { "Off", "CC", "Note", "Toggle", "Velocity" }, BtVelocity, hiddenChoice));
        else if (isButton (c))
            params.push_back (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { paramId (c, "Type"), 1 }, name + " Type",
                juce::StringArray { "Off", "CC", "Note", "Toggle" }, BtCC, hiddenChoice));
        else
            params.push_back (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { paramId (c, "Type"), 1 }, name + " Type",
                juce::StringArray { "Off", "CC", "CC 14-bit" }, CtOff, hiddenChoice));

        params.push_back (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { paramId (c, "Chan"), 1 }, name + " Channel", 1, 16, 1, hiddenInt));
        params.push_back (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { paramId (c, "Num"), 1 }, name + " Number", 0, 127, d.defNum, hiddenInt));

        if (c == Face)
        {
            // FACE value table: what each resting face emits, in % of the MIDI
            // course. The 0.01 % step matters: in CC 14-bit it keeps ~10000 of
            // the 16384 reachable values, where CC units would cap at 128.
            for (int f = 0; f < kNumFaces; ++f)
                params.push_back (std::make_unique<juce::AudioParameterFloat> (
                    juce::ParameterID { faceParamId (f), 1 },
                    name + " " + kFaceNames[f],
                    juce::NormalisableRange<float> (0.0f, 100.0f, 0.01f),
                    (float) f * (100.0f / (float) (kNumFaces - 1)),
                    juce::AudioParameterFloatAttributes{}.withAutomatable (false).withLabel ("%")));
        }
        else if (! isButton (c))
        {
            const float step = (d.hi - d.lo) > 100.0f ? 1.0f : 0.01f;
            params.push_back (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { paramId (c, "Min"), 1 }, name + " Min",
                juce::NormalisableRange<float> (d.lo, d.hi, step), d.defMin,
                juce::AudioParameterFloatAttributes{}.withAutomatable (false).withLabel (d.unit)));
            params.push_back (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { paramId (c, "Max"), 1 }, name + " Max",
                juce::NormalisableRange<float> (d.lo, d.hi, step), d.defMax,
                juce::AudioParameterFloatAttributes{}.withAutomatable (false).withLabel (d.unit)));
            params.push_back (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { paramId (c, "Bipolar"), 1 }, name + " Bipolar",
                true, hiddenBool));
        }
    }
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { juce::String (kPrefix) + "Deadzone", 1 }, "CIS Deadzone",
        juce::NormalisableRange<float> (0.0f, 20.0f, 0.5f), 2.0f,
        juce::AudioParameterFloatAttributes{}.withAutomatable (false).withLabel ("%")));
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { juce::String (kPrefix) + "SmoothMs", 1 }, "CIS Smoothing",
        juce::NormalisableRange<float> (0.0f, 500.0f, 1.0f, 0.5f), 30.0f,
        juce::AudioParameterFloatAttributes{}.withAutomatable (false).withLabel ("ms")));
    juce::ignoreUnused (hiddenFloat);
}

void HidMidiMapper::attach (juce::AudioProcessorValueTreeState& apvts)
{
    for (int c = 0; c < NumControls; ++c)
    {
        auto& r = raw_[(size_t) c];
        r.type = apvts.getRawParameterValue (paramId (c, "Type"));
        r.chan = apvts.getRawParameterValue (paramId (c, "Chan"));
        r.num  = apvts.getRawParameterValue (paramId (c, "Num"));
        if (! isButton (c) && c != Face)
        {
            r.min     = apvts.getRawParameterValue (paramId (c, "Min"));
            r.max     = apvts.getRawParameterValue (paramId (c, "Max"));
            r.bipolar = apvts.getRawParameterValue (paramId (c, "Bipolar"));
        }
        lastSent_[(size_t) c] = -1;
        smoothed_[(size_t) c] = 0.5f;
    }
    for (int f = 0; f < kNumFaces; ++f)
        faceVal_[(size_t) f] = apvts.getRawParameterValue (faceParamId (f));
    deadzone_ = apvts.getRawParameterValue (juce::String (kPrefix) + "Deadzone");
    smoothMs_ = apvts.getRawParameterValue (juce::String (kPrefix) + "SmoothMs");
}

//==============================================================================
bool HidMidiMapper::hidAlive() const noexcept
{
    return slp_hid_age_ms() < 500u;
}

bool HidMidiMapper::currentEvent (int c, int& type, int& channel, int& number) const noexcept
{
    const auto& r = raw_[(size_t) c];
    if (r.type == nullptr) return false;
    const int t = (int) r.type->load (std::memory_order_relaxed);
    if (t == 0) return false;
    channel = (int) r.chan->load (std::memory_order_relaxed);
    number  = (int) r.num ->load (std::memory_order_relaxed);
    type    = (isButton (c) && t == BtNote) ? 1 : 2;
    return true;
}

void HidMidiMapper::emitButton (int c, bool pressed, juce::MidiBuffer& out) noexcept
{
    const auto& r = raw_[(size_t) c];
    const int t  = (int) r.type->load (std::memory_order_relaxed);
    const int ch = juce::jlimit (1, 16, (int) r.chan->load (std::memory_order_relaxed));
    const int n  = juce::jlimit (0, 127, (int) r.num->load (std::memory_order_relaxed));
    switch (t)
    {
        case BtCC:
            out.addEvent (juce::MidiMessage::controllerEvent (ch, n, pressed ? 127 : 0), 0);
            break;
        case BtNote:
            out.addEvent (pressed ? juce::MidiMessage::noteOn (ch, n, (juce::uint8) 100)
                                  : juce::MidiMessage::noteOff (ch, n), 0);
            break;
        case BtToggle:
            if (pressed)
            {
                auto& tg = toggle_[(size_t) c];
                tg = ! tg;
                out.addEvent (juce::MidiMessage::controllerEvent (ch, n, tg ? 127 : 0), 0);
            }
            break;
        default: break;
    }
}

void HidMidiMapper::emitEventPulse (int c, int velocity, juce::MidiBuffer& out) noexcept
{
    const auto& r = raw_[(size_t) c];
    if (r.type == nullptr) return;
    const int t = (int) r.type->load (std::memory_order_relaxed);
    if (t == BtOff) return;
    const int ch = juce::jlimit (1, 16, (int) r.chan->load (std::memory_order_relaxed));
    const int n  = juce::jlimit (0, 127, (int) r.num->load (std::memory_order_relaxed));

    // Retrigger: a hit landing while the previous pulse is still held closes
    // it first. Without this, tapping faster than the pulse leaves a note on
    // for ever and the second strike is simply lost.
    if (auto& rel = release_[(size_t) c]; rel.ms > 0.0f)
    {
        if (rel.type == BtCC)        out.addEvent (juce::MidiMessage::controllerEvent (rel.chan, rel.num, 0), 0);
        else if (rel.type == BtNote) out.addEvent (juce::MidiMessage::noteOff (rel.chan, rel.num), 0);
        rel.ms = 0.0f;
    }

    switch (t)
    {
        case BtVelocity:
            // The strike strength, then straight back to 0: the control follows
            // the impulse and nothing more. Holding the value instead made a
            // percussive gesture behave like a knob left where you last hit it.
            out.addEvent (juce::MidiMessage::controllerEvent (ch, n, juce::jlimit (1, 127, velocity)), 0);
            release_[(size_t) c] = { kVelPulseMs, BtCC, ch, n };
            break;

        case BtCC:
            out.addEvent (juce::MidiMessage::controllerEvent (ch, n, 127), 0);
            release_[(size_t) c] = { kPulseMs, BtCC, ch, n };
            break;
        case BtNote:
            out.addEvent (juce::MidiMessage::noteOn (ch, n, (juce::uint8) juce::jlimit (1, 127, velocity)), 0);
            release_[(size_t) c] = { kPulseMs, BtNote, ch, n };
            break;
        case BtToggle:
        {
            auto& tg = toggle_[(size_t) c];
            tg = ! tg;
            out.addEvent (juce::MidiMessage::controllerEvent (ch, n, tg ? 127 : 0), 0);
            release_[(size_t) c] = { kPulseMs, BtOff, ch, n };   // meter blink only
            break;
        }
        default: break;
    }
    // The meter shows the FORCE of the blow, whatever the type: the page is
    // the only place you can see a light tap register at all.
    live_[(size_t) c].store ((float) juce::jlimit (0, 127, velocity) / 127.0f,
                             std::memory_order_relaxed);
    meterMs_[(size_t) c] = kMeterHoldMs;
}

void HidMidiMapper::process (juce::MidiBuffer& out, float blockMs) noexcept
{
    // Pending gesture releases tick on EVERY call, by the REAL block duration:
    // a stalled HID stream must never hold a note or a CC at 127.
    const float dtMsBlock = (blockMs > 0.0f && blockMs < 200.0f) ? blockMs : 3.0f;
    for (int c = Hit; c <= HitRight; ++c)
    {
        auto& rel = release_[(size_t) c];
        if (rel.ms > 0.0f && (rel.ms -= dtMsBlock) <= 0.0f)
        {
            if (rel.type == BtCC)        out.addEvent (juce::MidiMessage::controllerEvent (rel.chan, rel.num, 0), 0);
            else if (rel.type == BtNote) out.addEvent (juce::MidiMessage::noteOff (rel.chan, rel.num), 0);
            rel.ms = 0.0f;
        }
        auto& mm = meterMs_[(size_t) c];
        if (mm > 0.0f && (mm -= dtMsBlock) <= 0.0f)
        {
            live_[(size_t) c].store (0.0f, std::memory_order_relaxed);
            mm = 0.0f;
        }
    }

    slp_hid_sample s;
    const uint32_t gen = slp_hid_read (&s);
    if (gen == 0 || gen == lastGen_)
        return;
    lastGen_ = gen;

    // ── gesture events: u8 wrapping counter, immune to lost datagrams ────────
    if (! haveGestSeq_)
    {
        lastHitSeq_ = s.hit_seq;
        haveGestSeq_ = true;
    }
    else if (s.hit_seq != lastHitSeq_)
    {
        lastHitSeq_ = s.hit_seq;
        // One shock, two rows: HIT always, plus the row of the face it landed
        // on. A shock along the bar (SLP_FACE_MOVING) has no face row.
        emitEventPulse (Hit, s.hit_velocity, out);
        if (const int row = hitRowForFace (s.hit_face); row >= 0)
            emitEventPulse (row, s.hit_velocity, out);
    }

    // ── buttons: edge counters survive lost datagrams ────────────────────────
    for (int i = 0; i < kNumButtons; ++i)
    {
        const uint32_t seq  = s.button_seq[i];
        const bool pressed  = ((s.button_state >> i) & 1u) != 0;
        live_[(size_t) i].store (pressed ? 1.0f : 0.0f, std::memory_order_relaxed);

        if (! haveSeq_)
        {
            lastSeq_[(size_t) i] = seq;
            lastPressed_[(size_t) i] = pressed;
            continue;
        }
        uint32_t edges = seq - lastSeq_[(size_t) i];
        lastSeq_[(size_t) i] = seq;
        if (edges == 0) continue;
        if (edges > 4) edges = (edges & 1u) ? 3u : 2u;      // bound a synthetic burst

        bool st = lastPressed_[(size_t) i];
        for (uint32_t e = 0; e < edges; ++e) { st = ! st; emitButton (i, st, out); }
        if (st != pressed) { st = pressed; emitButton (i, st, out); }
        lastPressed_[(size_t) i] = st;
    }
    haveSeq_ = true;

    // ── continuous ───────────────────────────────────────────────────────────
    const float ax = s.acc[0], ay = s.acc[1], az = s.acc[2];
    const float pitch = std::atan2 (ax, std::sqrt (ay * ay + az * az)) * (180.0f / juce::MathConstants<float>::pi);
    const float roll  = std::atan2 (ay, std::fabs (az) + 1.0e-6f)      * (180.0f / juce::MathConstants<float>::pi);
    const float values[NumControls] = { 0, 0, 0, s.gyro[0], s.gyro[1], s.gyro[2], pitch, roll,
                                        0, 0, 0, 0, 0, (float) s.gesture_face };

    // Smoothing coefficient from the real HID period (timestamps), default 5 ms.
    float dtMs = kHidPeriodMs;
    if (lastTimestampUs_ != 0 && s.timestamp_us > lastTimestampUs_)
        dtMs = juce::jlimit (1.0f, 50.0f, (float) (s.timestamp_us - lastTimestampUs_) / 1000.0f);
    lastTimestampUs_ = s.timestamp_us;
    const float tau   = smoothMs_ ? smoothMs_->load (std::memory_order_relaxed) : 0.0f;
    const float alpha = tau <= 0.5f ? 1.0f : (1.0f - std::exp (-dtMs / tau));
    const float dz    = deadzone_ ? deadzone_->load (std::memory_order_relaxed) * 0.01f : 0.0f;

    // Rest gate: while the device lies still (gyro quiet for a while), the
    // gyro/tilt rows stop emitting - sensor noise crossing a CC step no longer
    // chatters, and MIDI learn stops being stolen at rest. A deliberate move
    // reopens the gate instantly.
    const float gyrMag = std::fabs (s.gyro[0]) + std::fabs (s.gyro[1]) + std::fabs (s.gyro[2]);
    if (gyrMag > kRestGateDps) restQuietMs_ = 0.0f;
    else if (restQuietMs_ < 1.0e6f) restQuietMs_ += dtMs;
    const bool resting = restQuietMs_ >= kRestGateMs;

    for (int c = kNumButtons; c < NumControls; ++c)
    {
        if (isButton (c)) continue;      // gesture events: handled above
        const auto& r = raw_[(size_t) c];
        if (r.type == nullptr) continue;
        float norm;
        if (c == Face)
        {
            // table lookup: the face picks its entry, % of the MIDI course
            const int f = juce::jlimit (0, kNumFaces - 1, (int) values[c]);
            auto* v = faceVal_[(size_t) f];
            norm = juce::jlimit (0.0f, 1.0f,
                                 (v ? v->load (std::memory_order_relaxed)
                                    : (float) f * (100.0f / (float) (kNumFaces - 1))) * 0.01f);
        }
        else
        {
            const float mn = r.min->load (std::memory_order_relaxed);
            const float mx = r.max->load (std::memory_order_relaxed);
            const float span = (mx - mn);
            norm = std::fabs (span) < 1.0e-6f ? 0.5f : (values[c] - mn) / span;
            norm = juce::jlimit (0.0f, 1.0f, norm);
            if (r.bipolar->load (std::memory_order_relaxed) > 0.5f && dz > 0.0f)
            {
                // dead band around the centre, then re-stretch so the full range stays reachable
                const float d = norm - 0.5f;
                const float half = dz * 0.5f;
                if (std::fabs (d) <= half) norm = 0.5f;
                else norm = 0.5f + (d - (d > 0 ? half : -half)) / (0.5f - half) * 0.5f;
            }
        }
        float& sm = smoothed_[(size_t) c];
        // FACE is a discrete selector: it snaps (smoothing would walk the CC
        // through every intermediate state).
        sm += (c == Face ? 1.0f : alpha) * (norm - sm);
        live_[(size_t) c].store (sm, std::memory_order_relaxed);

        const int t = (int) r.type->load (std::memory_order_relaxed);
        int& last = lastSent_[(size_t) c];
        if (t == CtOff) { last = -1; continue; }
        if (resting && c != Face) continue;   // rest gate (FACE has its own hysteresis)

        const int ch = juce::jlimit (1, 16, (int) r.chan->load (std::memory_order_relaxed));
        const int n  = juce::jlimit (0, 127, (int) r.num->load (std::memory_order_relaxed));
        if (t == CtCC)
        {
            const float v = sm * 127.0f;
            const int   q = juce::roundToInt (v);
            if (last < 0 || std::fabs (v - (float) last) > 0.6f)    // half-step hysteresis
            {
                out.addEvent (juce::MidiMessage::controllerEvent (ch, n, q), 0);
                last = q;
            }
        }
        else
        {
            const int q = juce::roundToInt (sm * 16383.0f);
            if (q != last)
            {
                out.addEvent (juce::MidiMessage::controllerEvent (ch, n, (q >> 7) & 0x7F), 0);
                if (n + 32 <= 127)
                    out.addEvent (juce::MidiMessage::controllerEvent (ch, n + 32, q & 0x7F), 0);
                last = q;
            }
        }
    }
}
