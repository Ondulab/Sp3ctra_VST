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
        { "AccX",  "ACC X",  "g",   -4.0f,   4.0f,   -1.0f,   1.0f,   30 },
        { "AccY",  "ACC Y",  "g",   -4.0f,   4.0f,   -1.0f,   1.0f,   31 },
        { "AccZ",  "ACC Z",  "g",   -4.0f,   4.0f,   -1.0f,   1.0f,   32 },
        { "GyrX",  "GYRO X", "dps", -2000.f, 2000.f, -250.f,  250.f,  33 },
        { "GyrY",  "GYRO Y", "dps", -2000.f, 2000.f, -250.f,  250.f,  34 },
        { "GyrZ",  "GYRO Z", "dps", -2000.f, 2000.f, -250.f,  250.f,  35 },
        { "TiltP", "TILT P", "deg", -90.f,   90.f,   -45.f,   45.f,   40 },
        { "TiltR", "TILT R", "deg", -90.f,   90.f,   -45.f,   45.f,   41 },
    };

    constexpr float kHidPeriodMs = 5.0f;      // 200 Hz default rate
}

const char* HidMidiMapper::controlKey  (int c) noexcept { return kDefs[c].key; }
const char* HidMidiMapper::controlName (int c) noexcept { return kDefs[c].name; }
const char* HidMidiMapper::controlUnit (int c) noexcept { return kDefs[c].unit; }
juce::String HidMidiMapper::paramId (int c, const char* suffix) { return juce::String (kPrefix) + kDefs[c].key + suffix; }

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
        if (isButton (c))
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

        if (! isButton (c))
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
                juce::ParameterID { paramId (c, "Bipolar"), 1 }, name + " Bipolar", true, hiddenBool));
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
        if (! isButton (c))
        {
            r.min     = apvts.getRawParameterValue (paramId (c, "Min"));
            r.max     = apvts.getRawParameterValue (paramId (c, "Max"));
            r.bipolar = apvts.getRawParameterValue (paramId (c, "Bipolar"));
        }
        lastSent_[(size_t) c] = -1;
        smoothed_[(size_t) c] = 0.5f;
    }
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

void HidMidiMapper::process (juce::MidiBuffer& out) noexcept
{
    slp_hid_sample s;
    const uint32_t gen = slp_hid_read (&s);
    if (gen == 0 || gen == lastGen_)
        return;
    lastGen_ = gen;

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
    const float values[NumControls] = { 0, 0, 0, ax, ay, az, s.gyro[0], s.gyro[1], s.gyro[2], pitch, roll };

    // Smoothing coefficient from the real HID period (timestamps), default 5 ms.
    float dtMs = kHidPeriodMs;
    if (lastTimestampUs_ != 0 && s.timestamp_us > lastTimestampUs_)
        dtMs = juce::jlimit (1.0f, 50.0f, (float) (s.timestamp_us - lastTimestampUs_) / 1000.0f);
    lastTimestampUs_ = s.timestamp_us;
    const float tau   = smoothMs_ ? smoothMs_->load (std::memory_order_relaxed) : 0.0f;
    const float alpha = tau <= 0.5f ? 1.0f : (1.0f - std::exp (-dtMs / tau));
    const float dz    = deadzone_ ? deadzone_->load (std::memory_order_relaxed) * 0.01f : 0.0f;

    for (int c = kNumButtons; c < NumControls; ++c)
    {
        const auto& r = raw_[(size_t) c];
        if (r.type == nullptr) continue;
        const float mn = r.min->load (std::memory_order_relaxed);
        const float mx = r.max->load (std::memory_order_relaxed);
        const float span = (mx - mn);
        float norm = std::fabs (span) < 1.0e-6f ? 0.5f : (values[c] - mn) / span;
        norm = juce::jlimit (0.0f, 1.0f, norm);
        if (r.bipolar->load (std::memory_order_relaxed) > 0.5f && dz > 0.0f)
        {
            // dead band around the centre, then re-stretch so the full range stays reachable
            const float d = norm - 0.5f;
            const float half = dz * 0.5f;
            if (std::fabs (d) <= half) norm = 0.5f;
            else norm = 0.5f + (d - (d > 0 ? half : -half)) / (0.5f - half) * 0.5f;
        }
        float& sm = smoothed_[(size_t) c];
        sm += alpha * (norm - sm);
        live_[(size_t) c].store (sm, std::memory_order_relaxed);

        const int t = (int) r.type->load (std::memory_order_relaxed);
        int& last = lastSent_[(size_t) c];
        if (t == CtOff) { last = -1; continue; }

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
