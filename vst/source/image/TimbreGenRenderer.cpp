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
        // textures
        double driftCents, brightRamp, unisonCents;
        double bodyHz, bodyDb;
        double burstDb, burstMs, burstTilt;
        double noiseDb, noiseTilt;
        const char* family = "Synthesizers";
        const char* subfamily = "Basic waveforms";
        int suggestedNote = 57;
        Spectrum spectrum = Spectrum::Neutral;
        double cutoffHz = 0.0, filterSweepOct = 0.0, filterDecayMs = 180.0;
        double pitchAttackCents = 0.0, pitchSettleMs = 35.0;
    };

    constexpr double OFF = kOffDb;

    // Slope/odd values follow the Fourier series of the ideal waveforms
    // (square = odd 1/n → −6 dB/oct odd-only; triangle = odd 1/n² → −12 dB/oct);
    // the acoustic instruments are starting points meant to be tweaked. The
    // synth waveforms stay CLEAN (no texture): their whole point is the ruler-
    // straight line. New presets are APPENDED — the preset index is persisted
    // (timbreGenState / midiScoreGenState). Retuning an existing row is safe:
    // a saved state carries its values, a template is only re-read on pick.
    const PresetDef kLegacyPresets[] =
    {
        //  name              nPart slope  odd   inharm  comb  pos   atk    dec   hf    bell  tbl  vibC rate  onset life | drift bright uni  bodyHz bodyDb burst  bMs  bTilt  noise nTilt
        { "Sine",                1,  0.0, 0.0, 0.0,     0.0, 0.28,   5.0,  0.0, 0.0,  false, 0,   0.0, 5.5, 0.40, 0.50,   0.0,  0.0,  0.0,    0.0,  0.0,   OFF,  8.0,  0.0,   OFF, -3.0 },
        { "Square",             40, -6.0, 1.0, 0.0,     0.0, 0.28,   3.0,  0.0, 0.0,  false, 0,   0.0, 5.5, 0.40, 0.50,   0.0,  0.0,  0.0,    0.0,  0.0,   OFF,  8.0,  0.0,   OFF, -3.0 },
        { "Triangle",           24, -12.0,1.0, 0.0,     0.0, 0.28,   3.0,  0.0, 0.0,  false, 0,   0.0, 5.5, 0.40, 0.50,   0.0,  0.0,  0.0,    0.0,  0.0,   OFF,  8.0,  0.0,   OFF, -3.0 },
        { "Sawtooth",           40, -6.0, 0.0, 0.0,     0.0, 0.28,   3.0,  0.0, 0.0,  false, 0,   0.0, 5.5, 0.40, 0.50,   0.0,  0.0,  0.0,    0.0,  0.0,   OFF,  8.0,  0.0,   OFF, -3.0 },
        // Pipes: a breath of wind noise, a hair of drift — never a synth.
        { "Organ",               8, -2.5, 0.0, 0.0,     0.0, 0.28,  10.0,  0.0, 0.0,  false, 0,   0.0, 5.5, 0.40, 0.50,   1.5,  0.0,  0.0,    0.0,  0.0,   OFF,  8.0,  0.0, -40.0, -3.0 },
        // Reed: the highs come in after the fundamental, breath in the sound.
        { "Clarinet",           20, -8.0, 0.8, 0.0,     0.0, 0.28,  30.0,  0.0, 0.1,  false, 0,   8.0, 5.0, 0.60, 0.50,   3.0,  0.3,  0.0,    0.0,  0.0, -44.0,  6.0, -6.0, -34.0,  1.0 },
        // Brass BLOOMS: the upper partials arrive well after the tongue.
        { "Brass",              30, -3.5, 0.0, 0.0,     0.0, 0.28,  60.0,  0.0, 0.1,  false, 0,  12.0, 4.8, 0.70, 0.55,   2.0,  0.6,  0.0, 1200.0,  3.0, -36.0,  8.0, -6.0, -40.0, -2.0 },
        // The pick is a bright click; highs first.
        { "E. Guitar (pluck)",  32, -5.0, 0.0, 3.0e-4,  0.8, 0.13,   2.0,  2.5, 0.7,  false, 0,   0.0, 5.5, 0.40, 0.50,   0.0, -0.3,  0.0,    0.0,  0.0, -18.0,  5.0,  3.0,   OFF, -3.0 },
        // Tine + hammer: a dull thud at the onset.
        { "E. Piano",           16, -9.0, 0.0, 1.0e-4,  0.3, 0.10,   2.0,  3.5, 0.8,  false, 0,   0.0, 5.5, 0.40, 0.50,   0.0, -0.3,  0.0,    0.0,  0.0, -26.0,  5.0, -3.0,   OFF, -3.0 },
        { "Bell (church)",      12,  0.0, 0.0, 0.0,     0.0, 0.28,   1.0,  6.0, 0.4,  true,  0,   0.0, 5.5, 0.40, 0.50,   0.0,  0.0,  0.0,    0.0,  0.0, -30.0,  4.0,  0.0,   OFF, -3.0 },
        { "Bell (glocken)",      4,  0.0, 0.0, 0.0,     0.0, 0.28,   1.0,  3.0, 0.5,  true,  1,   0.0, 5.5, 0.40, 0.50,   0.0,  0.0,  0.0,    0.0,  0.0, -24.0,  3.0,  6.0,   OFF, -3.0 },
        // Bowed string: harmonic series, bow-position comb (Helmholtz dips),
        // slow bow start, sustained — the singing vibrato, bow hair in the
        // sound, the wooden body lifting the low mids.
        { "Violin",             32, -4.5, 0.0, 0.0,     0.35,0.11,  90.0,  0.0, 0.0,  false, 0,  30.0, 5.5, 0.50, 0.65,   4.0,  0.4,  0.0,  300.0,  4.0,   OFF,  8.0,  0.0, -30.0, -2.0 },
        // ── Appended 2026-08-30: the textured family ──────────────────────
        // Three strings per note a few cents apart: the piano's own chorus.
        // The hammer is a soft thud; the highs speak first, then die first.
        { "Piano",              40, -7.0, 0.0, 3.0e-4,  0.5, 0.12,   2.0,  5.0, 0.75, false, 0,   0.0, 5.5, 0.40, 0.50,   0.0, -0.4,  3.0,  180.0,  3.0, -24.0,  5.0, -3.0,   OFF, -3.0 },
        { "Cello",              32, -4.0, 0.0, 0.0,     0.4, 0.10, 120.0,  0.0, 0.0,  false, 0,  25.0, 5.0, 0.60, 0.70,   5.0,  0.4,  0.0,  220.0,  5.0,   OFF,  8.0,  0.0, -28.0, -3.0 },
        // A whole section: the players never quite agree.
        { "Strings (section)",  28, -4.5, 0.0, 0.0,     0.3, 0.11, 150.0,  0.0, 0.0,  false, 0,  20.0, 5.3, 0.60, 0.90,   6.0,  0.4, 12.0,  300.0,  3.0,   OFF,  8.0,  0.0, -34.0, -2.0 },
        // Breath IS the flute: few partials, bright noise, a lively line.
        { "Flute",               8, -10.0,0.0, 0.0,     0.0, 0.28,  60.0,  0.0, 0.0,  false, 0,  20.0, 5.2, 0.50, 0.60,   6.0,  0.5,  0.0,    0.0,  0.0, -40.0, 10.0,  2.0, -24.0,  1.0 },
        // "ah": the first vowel formant lifts the partials near 800 Hz on
        // every note, so the same voice sings a different colour per pitch.
        { "Voice (ah)",         30, -6.0, 0.0, 0.0,     0.0, 0.28,  40.0,  0.0, 0.0,  false, 0,  40.0, 5.5, 0.40, 0.70,   8.0,  0.2,  0.0,  800.0,  8.0,   OFF,  8.0,  0.0, -36.0,  0.0 },
        // Chorused saw pad: wide unison, slow rise, a slow wander.
        { "Pad (chorus)",       30, -6.0, 0.0, 0.0,     0.0, 0.28, 300.0,  0.0, 0.0,  false, 0,   0.0, 5.5, 0.40, 0.50,   4.0,  0.2, 14.0,    0.0,  0.0,   OFF,  8.0,  0.0,   OFF, -3.0 },
        { "Tubular bell",       10,  0.0, 0.0, 0.0,     0.0, 0.28,   1.0,  8.0, 0.3,  true,  2,   0.0, 5.5, 0.40, 0.50,   0.0,  0.0,  0.0,    0.0,  0.0, -28.0,  3.0,  3.0,   OFF, -3.0 },
        { "Marimba",             3,  0.0, 0.0, 0.0,     0.0, 0.28,   1.0,  1.2, 0.6,  true,  3,   0.0, 5.5, 0.40, 0.50,   0.0,  0.0,  0.0,    0.0,  0.0, -20.0,  4.0,  0.0,   OFF, -3.0 },
        // Drum heads: the Bessel modes of a circular membrane, a thud at
        // the strike; the snare adds its bright rattle.
        { "Membrane (tom)",     11,  0.0, 0.0, 0.0,     0.0, 0.28,   1.0,  0.8, 0.5,  true,  4,   0.0, 5.5, 0.40, 0.50,   0.0,  0.0,  0.0,    0.0,  0.0, -14.0,  6.0, -6.0, -40.0, -6.0 },
        { "Snare",              11,  0.0, 0.0, 0.0,     0.0, 0.28,   1.0,  0.35,0.4,  true,  4,   0.0, 5.5, 0.40, 0.50,   0.0,  0.0,  0.0,    0.0,  0.0,  -6.0,  4.0,  4.0,  -8.0,  2.0 },
        // No partial stack at all: a bright grain that dies fast.
        { "Hi-hat",              0,  0.0, 0.0, 0.0,     0.0, 0.28,   1.0,  0.25,0.0,  false, 0,   0.0, 5.5, 0.40, 0.50,   0.0,  0.0,  0.0,    0.0,  0.0,  -2.0,  3.0,  6.0,  -4.0,  6.0 },
        { "Cymbal",             16,  0.0, 0.0, 0.0,     0.0, 0.28,   1.0,  4.0, 0.3,  true,  5,   0.0, 5.5, 0.40, 0.50,   0.0,  0.0,  0.0,    0.0,  0.0, -10.0,  4.0,  3.0, -14.0,  3.0 },
        // The gong blooms: its upper modes take a good second to speak.
        { "Gong",               13,  0.0, 0.0, 0.0,     0.0, 0.28,  30.0, 10.0, 0.2,  true,  6,   0.0, 5.5, 0.40, 0.50,   3.0,  0.8,  0.0,    0.0,  0.0, -26.0,  8.0, -3.0, -34.0, -3.0 },
    };

    // Factory library: existing IDs 0..24 are stable. New sounds append;
    // presentation order is determined by family/subfamily, never by the ID.
    const std::vector<PresetDef> kPresets = []
    {
        std::vector<PresetDef> v(std::begin(kLegacyPresets), std::end(kLegacyPresets));
        auto classify = [&](int i, const char* family, const char* sub, Spectrum shape, int note)
        {
            auto& d = v[(size_t)i];
            d.family = family; d.subfamily = sub; d.spectrum = shape; d.suggestedNote = note;
            // Broadband texture spans hundreds of reader oscillators. Keep
            // tonal patches restrained; percussion keeps its deliberate noise.
            if (i < 20 && d.noiseDb > OFF) d.noiseDb = juce::jmin(d.noiseDb, -46.0);
            if (i < 20 && d.burstDb > OFF) d.burstDb = juce::jmin(d.burstDb, -36.0);
        };
        classify(4, "Keys", "Organs", Spectrum::Tonewheel, 57);
        classify(5, "Woodwinds", "Single reeds", Spectrum::Clarinet, 57);
        classify(6, "Brass", "Ensembles", Spectrum::Brass, 53);
        classify(7, "Guitars & plucked", "Electric guitars", Spectrum::Plucked, 52);
        classify(8, "Keys", "Electric pianos", Spectrum::Tine, 57);
        classify(9, "Percussion", "Bells", Spectrum::Neutral, 60);
        classify(10, "Percussion", "Mallets", Spectrum::Neutral, 72);
        classify(11, "Strings", "Solo bowed", Spectrum::Bowed, 67);
        classify(12, "Keys", "Acoustic pianos", Spectrum::Piano, 57);
        classify(13, "Strings", "Solo bowed", Spectrum::Bowed, 48);
        classify(14, "Strings", "Ensembles", Spectrum::Bowed, 57);
        classify(15, "Woodwinds", "Flutes", Spectrum::Flute, 72);
        classify(16, "Voices", "Solo vowels", Spectrum::VoiceAh, 57);
        classify(17, "Synthesizers", "Pads", Spectrum::Neutral, 57);
        classify(18, "Percussion", "Bells", Spectrum::Neutral, 60);
        classify(19, "Percussion", "Mallets", Spectrum::Neutral, 60);
        classify(20, "Percussion", "Drums", Spectrum::Neutral, 45);
        classify(21, "Percussion", "Drums", Spectrum::Neutral, 50);
        classify(22, "Percussion", "Cymbals", Spectrum::Neutral, 78);
        classify(23, "Percussion", "Cymbals", Spectrum::Neutral, 66);
        classify(24, "Percussion", "Gongs", Spectrum::Neutral, 45);
        v[4].name = "Drawbar organ"; v[4].numPartials = 9; v[4].slopeDbPerOct = 0.0;
        v[5].oddBias = 0.42; v[5].slopeDbPerOct = -4.0; v[5].cutoffHz = 5200.0;
        v[6].name = "Brass ensemble"; v[6].slopeDbPerOct = -2.0;
        v[6].cutoffHz = 2200.0; v[6].filterSweepOct = 1.2; v[6].filterDecayMs = 300.0;
        v[7].name = "Electric guitar - clean"; v[7].inharmonicity = 0.00002;
        v[7].combDepth = 0.35; v[7].cutoffHz = 2800.0; v[7].filterSweepOct = 1.4;
        v[7].decaySec = 4.0; v[7].pitchAttackCents = 7.0;
        v[8].name = "Tine electric piano"; v[8].slopeDbPerOct = -3.0;
        v[8].combDepth = 0.0; v[8].inharmonicity = 0.00001; v[8].cutoffHz = 5000.0;
        v[9].name = "Church bell"; v[10].name = "Glockenspiel";
        v[11].vibCents = 18.0; v[11].driftCents = 1.5; v[11].cutoffHz = 6500.0;
        v[12].name = "Grand piano"; v[12].slopeDbPerOct = -4.5; v[12].inharmonicity = 0.00006;
        v[12].combDepth = 0.2; v[12].unisonCents = 1.8; v[12].cutoffHz = 3800.0;
        v[12].filterSweepOct = 1.5; v[12].filterDecayMs = 260.0; v[12].decaySec = 6.0;
        v[13].vibCents = 15.0; v[13].driftCents = 1.5; v[13].cutoffHz = 3800.0;
        v[14].vibCents = 12.0; v[14].driftCents = 2.5; v[14].unisonCents = 7.0;
        v[14].cutoffHz = 4800.0;
        v[15].slopeDbPerOct = -2.0; v[15].vibCents = 12.0; v[15].driftCents = 1.5;
        v[15].cutoffHz = 7000.0; v[15].noiseTilt = -3.0;
        v[16].name = "Vocal ah"; v[16].bodyHz = 0.0; v[16].bodyDb = 0.0;
        v[16].slopeDbPerOct = -3.0; v[16].vibCents = 18.0; v[16].driftCents = 2.0;
        v[16].noiseDb = OFF; v[16].attackMs = 65.0; v[16].cutoffHz = 5200.0;
        // Soft mallet: the bar modes carry the attack, not a white-noise click.
        v[19].burstDb = OFF; v[19].attackMs = 7.0; v[19].brightRamp = 0.25;
        v[19].cutoffHz = 3600.0; v[19].filterSweepOct = 0.4;
        v[19].filterDecayMs = 90.0; v[19].decaySec = 2.0; v[19].hfDamp = 0.8;
        v[17].name = "Chorus pad"; v[17].cutoffHz = 2800.0; v[17].unisonCents = 9.0;
        v[20].name = "Tom"; v[20].pitchAttackCents = 180.0; v[20].pitchSettleMs = 35.0;
        v[21].pitchAttackCents = 90.0; v[21].pitchSettleMs = 20.0;
        v[22].name = "Closed hi-hat";

        auto add = [&](int base, const char* name, const char* family, const char* sub,
                       Spectrum shape, int note) -> PresetDef&
        {
            auto d = v[(size_t)base];
            d.name = name; d.family = family; d.subfamily = sub;
            d.spectrum = shape; d.suggestedNote = note;
            v.push_back(d);
            return v.back();
        };
        { // 25
            auto& d = add(7, "Finger bass", "Bass", "Electric", Spectrum::FingerBass, 40);
            d.numPartials = 36; d.slopeDbPerOct = -4.5; d.combDepth = 0.12;
            d.combPos = 0.22; d.attackMs = 7; d.decaySec = 5.5;
            d.hfDamp = 0.55; d.cutoffHz = 900; d.filterSweepOct = 1.8;
            d.filterDecayMs = 140; d.bodyHz = 160; d.bodyDb = 2;
            d.burstDb = -44; d.pitchAttackCents = 5;
        }
        { // 26
            auto& d = add(25, "Pick bass", "Bass", "Electric", Spectrum::PickBass, 40);
            d.attackMs = 2; d.slopeDbPerOct = -3.5; d.cutoffHz = 1700;
            d.filterSweepOct = 1.5; d.filterDecayMs = 100; d.burstDb = -35;
            d.burstTilt = -2; d.combPos = 0.12;
        }
        { // 27
            auto& d = add(25, "Slap bass", "Bass", "Electric", Spectrum::SlapBass, 40);
            d.attackMs = 1; d.cutoffHz = 1300; d.filterSweepOct = 3;
            d.filterDecayMs = 70; d.burstDb = -29; d.burstMs = 3;
            d.burstTilt = 1; d.pitchAttackCents = 24; d.pitchSettleMs = 18;
            d.hfDamp = 0.8;
        }
        { // 28
            auto& d = add(25, "Muted bass", "Bass", "Electric", Spectrum::FingerBass, 40);
            d.attackMs = 5; d.decaySec = 1.5; d.cutoffHz = 450;
            d.filterSweepOct = 1.2; d.filterDecayMs = 55; d.hfDamp = 0.9;
            d.burstDb = -48;
        }
        { // 29
            auto& d = add(25, "Fretless bass", "Bass", "Electric", Spectrum::FingerBass, 40);
            d.attackMs = 22; d.decaySec = 6; d.cutoffHz = 1300;
            d.filterSweepOct = 0.4; d.pitchAttackCents = -32; d.pitchSettleMs = 70;
            d.vibCents = 8; d.vibOnsetSec = 0.65; d.bodyHz = 360;
            d.bodyDb = 3;
        }
        { // 30
            auto& d = add(25, "Upright bass - pizzicato", "Bass", "Acoustic", Spectrum::Plucked, 40);
            d.attackMs = 12; d.decaySec = 3.2; d.cutoffHz = 1400;
            d.filterSweepOct = 1; d.bodyHz = 180; d.bodyDb = 4;
            d.combDepth = 0.4; d.combPos = 0.26; d.burstDb = -40;
            d.burstTilt = -6; d.pitchAttackCents = 12;
        }
        { // 31
            auto& d = add(13, "Double bass - bowed", "Bass", "Acoustic", Spectrum::Bowed, 40);
            d.attackMs = 160; d.cutoffHz = 2400; d.bodyHz = 120;
            d.bodyDb = 4; d.vibCents = 7; d.slopeDbPerOct = -5;
            d.noiseDb = -48;
        }
        { // 32
            auto& d = add(0, "Sub bass", "Bass", "Synth", Spectrum::Neutral, 40);
            d.numPartials = 3; d.slopeDbPerOct = -18; d.attackMs = 9;
            d.cutoffHz = 240; d.filterSweepOct = 0; d.decaySec = 0;
        }
        { // 33
            auto& d = add(3, "Analog bass", "Bass", "Synth", Spectrum::Neutral, 40);
            d.attackMs = 4; d.cutoffHz = 550; d.filterSweepOct = 3; d.slopeDbPerOct = -3.0;
            d.filterDecayMs = 160; d.decaySec = 4; d.hfDamp = 0.2;
        }
        { // 34
            auto& d = add(1, "Square bass", "Bass", "Synth", Spectrum::Neutral, 40);
            d.attackMs = 5; d.cutoffHz = 750; d.filterSweepOct = 2;
            d.filterDecayMs = 110; d.decaySec = 3;
        }
        { // 35
            auto& d = add(8, "FM bass", "Bass", "Synth", Spectrum::FM, 40);
            d.attackMs = 2; d.numPartials = 28; d.slopeDbPerOct = -2;
            d.cutoffHz = 850; d.filterSweepOct = 3.5; d.filterDecayMs = 90;
            d.decaySec = 3.2; d.hfDamp = 0.9;
        }
        { // 36
            auto& d = add(0, "808 bass", "Bass", "Synth", Spectrum::Neutral, 40);
            d.numPartials = 5; d.slopeDbPerOct = -16; d.attackMs = 2;
            d.decaySec = 7; d.hfDamp = 0.65; d.cutoffHz = 350;
            d.pitchAttackCents = 900; d.pitchSettleMs = 22;
        }
        { // 37
            auto& d = add(7, "Nylon guitar", "Guitars & plucked", "Acoustic guitars", Spectrum::Plucked, 52);
            d.slopeDbPerOct = -5; d.cutoffHz = 2200; d.filterSweepOct = 1.2;
            d.combPos = 0.24; d.combDepth = 0.55; d.bodyHz = 190;
            d.bodyDb = 4; d.attackMs = 5; d.burstDb = -43;
            d.decaySec = 4.5;
        }
        { // 38
            auto& d = add(37, "Steel-string guitar", "Guitars & plucked", "Acoustic guitars", Spectrum::Plucked, 52);
            d.slopeDbPerOct = -3.5; d.cutoffHz = 3800; d.filterSweepOct = 1.3;
            d.combPos = 0.15; d.burstDb = -36; d.burstTilt = 0;
            d.unisonCents = 1.2;
        }
        { // 39
            auto& d = add(7, "Jazz guitar", "Guitars & plucked", "Electric guitars", Spectrum::Plucked, 52);
            d.cutoffHz = 1200; d.filterSweepOct = 0.7; d.attackMs = 6;
            d.slopeDbPerOct = -6; d.decaySec = 3.5; d.burstDb = -48;
        }
        { // 40
            auto& d = add(37, "Harp", "Guitars & plucked", "Harp & folk", Spectrum::Plucked, 57);
            d.cutoffHz = 3200; d.filterSweepOct = 0.8; d.decaySec = 6;
            d.combPos = 0.32; d.bodyHz = 250; d.bodyDb = 2;
            d.burstDb = -48; d.pitchAttackCents = 0;
        }
        { // 41
            auto& d = add(38, "Mandolin", "Guitars & plucked", "Harp & folk", Spectrum::Plucked, 67);
            d.cutoffHz = 5200; d.attackMs = 2; d.decaySec = 2;
            d.unisonCents = 4; d.bodyHz = 480; d.bodyDb = 3;
            d.combPos = 0.1;
        }
        { // 42
            auto& d = add(12, "Felt piano", "Keys", "Acoustic pianos", Spectrum::Piano, 57);
            d.cutoffHz = 1400; d.filterSweepOct = 0.7; d.attackMs = 9;
            d.slopeDbPerOct = -7; d.burstDb = -48; d.decaySec = 4.5;
        }
        { // 43
            auto& d = add(12, "Bright piano", "Keys", "Acoustic pianos", Spectrum::Piano, 57);
            d.cutoffHz = 6200; d.filterSweepOct = 0.8; d.slopeDbPerOct = -3;
            d.attackMs = 1.5; d.burstDb = -33; d.filterDecayMs = 180;
        }
        { // 44
            auto& d = add(8, "Reed electric piano", "Keys", "Electric pianos", Spectrum::Piano, 57);
            d.slopeDbPerOct = -4; d.cutoffHz = 2800; d.filterSweepOct = 1.2;
            d.decaySec = 3.2; d.bodyHz = 650; d.bodyDb = 3;
        }
        { // 45
            auto& d = add(7, "Clavinet", "Keys", "Electric pianos", Spectrum::PickBass, 57);
            d.cutoffHz = 3100; d.filterSweepOct = 1.7; d.filterDecayMs = 65;
            d.combDepth = 0.75; d.combPos = 0.08; d.decaySec = 1.8;
            d.burstDb = -36;
        }
        { // 46
            auto& d = add(7, "Harpsichord", "Keys", "Plucked keys", Spectrum::Plucked, 60);
            d.cutoffHz = 6500; d.filterSweepOct = 0.5; d.combDepth = 0.7;
            d.combPos = 0.07; d.decaySec = 2.4; d.slopeDbPerOct = -3;
            d.burstDb = -35; d.pitchAttackCents = 0;
        }
        { // 47
            auto& d = add(5, "Oboe", "Woodwinds", "Double reeds", Spectrum::DoubleReed, 64);
            d.oddBias = 0; d.slopeDbPerOct = -3.5; d.attackMs = 35;
            d.bodyHz = 1400; d.bodyDb = 4; d.cutoffHz = 6000;
            d.vibCents = 12;
        }
        { // 48
            auto& d = add(47, "Bassoon", "Woodwinds", "Double reeds", Spectrum::DoubleReed, 48);
            d.slopeDbPerOct = -5; d.attackMs = 55; d.bodyHz = 500;
            d.cutoffHz = 3000; d.vibCents = 7;
        }
        { // 49
            auto& d = add(5, "Tenor sax", "Woodwinds", "Single reeds", Spectrum::DoubleReed, 52);
            d.oddBias = 0.08; d.slopeDbPerOct = -4; d.bodyHz = 850;
            d.bodyDb = 4; d.cutoffHz = 4200; d.vibCents = 15;
            d.attackMs = 35; d.pitchAttackCents = -12; d.pitchSettleMs = 45;
        }
        { // 50
            auto& d = add(15, "Recorder", "Woodwinds", "Flutes", Spectrum::Flute, 72);
            d.attackMs = 22; d.vibCents = 2; d.driftCents = 0.5;
            d.noiseDb = -50; d.slopeDbPerOct = -3;
        }
        { // 51
            auto& d = add(6, "Trumpet", "Brass", "Solo brass", Spectrum::Brass, 60);
            d.unisonCents = 0; d.attackMs = 28; d.cutoffHz = 4200;
            d.filterSweepOct = 0.8; d.bodyHz = 1500; d.bodyDb = 3;
            d.vibCents = 9;
        }
        { // 52
            auto& d = add(51, "Trombone", "Brass", "Solo brass", Spectrum::Brass, 48);
            d.attackMs = 45; d.cutoffHz = 2400; d.bodyHz = 650;
            d.vibCents = 7; d.slopeDbPerOct = -3;
        }
        { // 53
            auto& d = add(51, "French horn", "Brass", "Solo brass", Spectrum::Brass, 53);
            d.attackMs = 90; d.cutoffHz = 1600; d.filterSweepOct = 0.5;
            d.bodyHz = 480; d.vibCents = 4; d.slopeDbPerOct = -5;
        }
        { // 54
            auto& d = add(51, "Tuba", "Brass", "Solo brass", Spectrum::Brass, 40);
            d.attackMs = 75; d.cutoffHz = 1100; d.filterSweepOct = 0.4;
            d.bodyHz = 220; d.vibCents = 3; d.slopeDbPerOct = -6;
        }
        { // 55
            auto& d = add(11, "Viola", "Strings", "Solo bowed", Spectrum::Bowed, 60);
            d.bodyHz = 380; d.cutoffHz = 4800; d.attackMs = 105;
            d.vibCents = 14;
        }
        { // 56
            auto& d = add(13, "Pizzicato strings", "Strings", "Plucked", Spectrum::Plucked, 55);
            d.attackMs = 4; d.decaySec = 1.5; d.hfDamp = 0.8;
            d.cutoffHz = 2400; d.filterSweepOct = 1.5; d.vibCents = 0;
            d.noiseDb = -60; d.driftCents = 0; d.burstDb = -42;
        }
        { // 57
            auto& d = add(16, "Vocal oo", "Voices", "Solo vowels", Spectrum::VoiceOo, 57);
            d.slopeDbPerOct = -4; d.cutoffHz = 4200;
        }
        { // 58
            auto& d = add(16, "Vocal ee", "Voices", "Solo vowels", Spectrum::VoiceEe, 57);
            d.slopeDbPerOct = -3; d.cutoffHz = 6000;
        }
        { // 59
            auto& d = add(16, "Choir ah", "Voices", "Choirs", Spectrum::VoiceAh, 57);
            d.attackMs = 180; d.unisonCents = 8; d.vibCents = 10;
            d.vibLife = 0.85; d.driftCents = 3; d.noiseDb = OFF;
        }
        { // 60
            auto& d = add(17, "Warm pad", "Synthesizers", "Pads", Spectrum::Neutral, 57);
            d.attackMs = 500; d.cutoffHz = 1400; d.filterSweepOct = -1.5;
            d.filterDecayMs = 800; d.unisonCents = 6; d.slopeDbPerOct = -7;
        }
        { // 61
            auto& d = add(8, "Glass pad", "Synthesizers", "Pads", Spectrum::FM, 64);
            d.attackMs = 260; d.decaySec = 0; d.cutoffHz = 6500;
            d.unisonCents = 5; d.brightRamp = 0.3; d.burstDb = -60;
        }
        { // 62
            auto& d = add(3, "Analog lead", "Synthesizers", "Leads", Spectrum::Neutral, 64);
            d.attackMs = 12; d.cutoffHz = 2600; d.filterSweepOct = 1.5;
            d.filterDecayMs = 220; d.vibCents = 8; d.vibOnsetSec = 0.35;
        }
        { // 63
            auto& d = add(1, "Soft square lead", "Synthesizers", "Leads", Spectrum::Neutral, 64);
            d.attackMs = 25; d.cutoffHz = 1900; d.vibCents = 10;
            d.vibOnsetSec = 0.25;
        }
        { // 64
            auto& d = add(0, "Kick drum", "Percussion", "Drums", Spectrum::Neutral, 40);
            d.numPartials = 4; d.slopeDbPerOct = -15; d.attackMs = 1;
            d.decaySec = 0.65; d.hfDamp = 0.7; d.pitchAttackCents = 1500;
            d.pitchSettleMs = 18; d.burstDb = -32; d.burstMs = 2;
            d.burstTilt = -9; d.cutoffHz = 700;
        }
        { // 65
            auto& d = add(10, "Vibraphone", "Percussion", "Mallets", Spectrum::Neutral, 65);
            d.attackMs = 4; d.decaySec = 5; d.unisonCents = 2;
            d.burstDb = -42; d.hfDamp = 0.25;
        }
        { // 66
            auto& d = add(19, "Kalimba", "Percussion", "Mallets", Spectrum::Neutral, 67);
            d.attackMs = 1; d.decaySec = 2.5; d.burstDb = -33;
            d.burstMs = 2; d.hfDamp = 0.8;
        }
        { // 67
            auto& d = add(22, "Open hi-hat", "Percussion", "Cymbals", Spectrum::Neutral, 78);
            d.decaySec = 1.1; d.noiseDb = -8; d.burstDb = -12;
        }
        return v;
    }();

    // Inharmonic partial tables. decayMul > 1 = dies faster than the prime.
    struct TablePartial { double ratio, ampDb, decayMul; };

    // Church bell: hum / prime / tierce (minor third!) / quint / nominal + uppers.
    const TablePartial kChurchBell[] =
    {
        { 0.50,  -3.0, 0.5 }, { 1.00,   0.0, 0.7 }, { 1.20,  -2.0, 1.0 },
        { 1.50,  -9.0, 1.3 }, { 2.00,  -3.0, 1.5 }, { 2.50,  -8.0, 1.8 },
        { 2.67, -12.0, 2.0 }, { 3.00, -11.0, 2.3 }, { 3.70, -14.0, 2.6 },
        { 4.20, -15.0, 3.0 }, { 5.40, -19.0, 3.5 }, { 6.80, -23.0, 4.2 },
    };

    // Ideal struck bar (glockenspiel / vibraphone family).
    const TablePartial kStruckBar[] =
    {
        { 1.00,   0.0, 1.0 }, { 2.76,  -7.0, 1.6 },
        { 5.40, -14.0, 2.5 }, { 8.93, -21.0, 3.6 },
    };

    // Chime: the free-bar modes with the hum below the strike note.
    const TablePartial kTubularBell[] =
    {
        { 0.50, -10.0, 0.6 }, { 1.00,   0.0, 0.8 }, { 1.62,  -4.0, 1.0 },
        { 2.76,  -2.0, 1.2 }, { 4.00,  -8.0, 1.6 }, { 5.40,  -6.0, 2.0 },
        { 7.20, -12.0, 2.5 }, { 8.93, -10.0, 3.0 }, { 13.3, -16.0, 4.0 },
        { 18.6, -20.0, 5.0 },
    };

    // Marimba bar, tuned by the arch so the overtones sit at 4× and 10×.
    const TablePartial kMarimba[] =
    {
        { 1.00,   0.0, 1.0 }, { 4.00, -11.0, 1.8 }, { 10.0, -24.0, 2.8 },
    };

    // Circular membrane: Bessel-function modes relative to (0,1).
    const TablePartial kMembrane[] =
    {
        { 1.00,   0.0, 1.0 }, { 1.59,  -4.0, 1.3 }, { 2.14,  -8.0, 1.6 },
        { 2.30,  -9.0, 1.7 }, { 2.65, -12.0, 2.0 }, { 2.92, -13.0, 2.2 },
        { 3.16, -15.0, 2.4 }, { 3.50, -17.0, 2.7 }, { 3.60, -18.0, 2.8 },
        { 3.65, -18.0, 2.8 }, { 4.06, -21.0, 3.2 },
    };

    // Cymbal: a dense, near-random cluster of plate modes, all about as loud.
    const TablePartial kCymbal[] =
    {
        { 1.00,  -2.0, 1.0 }, { 1.31,  -3.0, 1.1 }, { 1.72,  -2.0, 1.2 },
        { 2.09,  -4.0, 1.3 }, { 2.51,  -3.0, 1.4 }, { 2.94,  -5.0, 1.5 },
        { 3.37,  -4.0, 1.6 }, { 3.81,  -6.0, 1.7 }, { 4.48,  -5.0, 1.8 },
        { 5.13,  -7.0, 1.9 }, { 5.96,  -6.0, 2.0 }, { 6.74,  -8.0, 2.1 },
        { 7.83,  -8.0, 2.2 }, { 9.02, -10.0, 2.3 }, { 10.6, -10.0, 2.4 },
        { 12.4, -12.0, 2.5 },
    };

    // Gong / tam-tam: a hum, a dense low cluster, uppers that bloom late.
    const TablePartial kGong[] =
    {
        { 0.50,  -6.0, 0.5 }, { 1.00,   0.0, 0.7 }, { 1.26,  -3.0, 0.8 },
        { 1.58,  -4.0, 0.9 }, { 1.98,  -5.0, 1.0 }, { 2.36,  -7.0, 1.1 },
        { 2.83,  -8.0, 1.2 }, { 3.32, -10.0, 1.4 }, { 3.97, -12.0, 1.6 },
        { 4.71, -14.0, 1.8 }, { 5.60, -17.0, 2.0 }, { 6.70, -20.0, 2.3 },
        { 8.00, -23.0, 2.6 },
    };

    struct TableDef { const char* name; const TablePartial* rows; int n; };
    #define TBL(name, arr) { name, arr, (int) (sizeof(arr) / sizeof(arr[0])) }
    const TableDef kTables[] =
    {
        TBL("Church bell",  kChurchBell),
        TBL("Struck bar",   kStruckBar),
        TBL("Tubular bell", kTubularBell),
        TBL("Marimba",      kMarimba),
        TBL("Membrane",     kMembrane),
        TBL("Cymbal",       kCymbal),
        TBL("Gong",         kGong),
    };
    #undef TBL
}

int numPresets() { return (int) kPresets.size(); }
const char* presetFamily(int p) { return p >= 0 && p < numPresets() ? kPresets[(size_t)p].family : "Custom"; }
const char* presetSubfamily(int p) { return p >= 0 && p < numPresets() ? kPresets[(size_t)p].subfamily : "User sounds"; }
int presetSuggestedNote(int p) { return p >= 0 && p < numPresets() ? kPresets[(size_t)p].suggestedNote : 57; }

const char* spectrumName(int spectrum)
{
    static const char* names[] = { "Neutral", "Piano strings", "Electric tines", "Plucked strings",
        "Finger bass", "Picked bass", "Slap bass", "Bowed strings", "Flute pipe", "Clarinet reed",
        "Double reed", "Brass bell", "Vowel ah", "Vowel oo", "Vowel ee", "Tonewheel", "FM harmonics" };
    return names[juce::jlimit(0, (int) Spectrum::Count - 1, spectrum)];
}

const char* presetName(int preset)
{
    if (preset < 0 || preset >= numPresets())
        return "Custom";
    return kPresets[preset].name;
}

int numTables() { return (int) (sizeof(kTables) / sizeof(kTables[0])); }

const char* tableName(int table)
{
    if (table < 0 || table >= numTables())
        return "?";
    return kTables[table].name;
}

void applyPreset(TimbreSlotParams& p, int preset)
{
    if (preset < 0 || preset >= numPresets())
        return;
    const PresetDef& d = kPresets[preset];
    p.preset        = preset;
    p.customBase    = -1;
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
    p.driftCents    = d.driftCents;
    p.brightRamp    = d.brightRamp;
    p.unisonCents   = d.unisonCents;
    p.bodyHz        = d.bodyHz;
    p.bodyDb        = d.bodyDb;
    p.burstDb       = d.burstDb;
    p.burstMs       = d.burstMs;
    p.burstTilt     = d.burstTilt;
    p.noiseDb       = d.noiseDb;
    p.noiseTilt     = d.noiseTilt;
    p.spectrum      = (int) d.spectrum;
    p.cutoffHz      = d.cutoffHz;
    p.filterSweepOct = d.filterSweepOct;
    p.filterDecayMs = d.filterDecayMs;
    p.pitchAttackCents = d.pitchAttackCents;
    p.pitchSettleMs = d.pitchSettleMs;
}

//==============================================================================
// Persistence
//==============================================================================
void encodeParams(juce::DynamicObject& o, const TimbreSlotParams& q)
{
    o.setProperty("preset", q.preset);
    o.setProperty("cbase",  q.customBase);
    o.setProperty("part",   q.numPartials);
    o.setProperty("slope",  q.slopeDbPerOct);
    o.setProperty("odd",    q.oddBias);
    o.setProperty("inh",    q.inharmonicity);
    o.setProperty("comb",   q.combDepth);
    o.setProperty("cpos",   q.combPos);
    o.setProperty("atk",    q.attackMs);
    o.setProperty("dec",    q.decaySec);
    o.setProperty("hf",     q.hfDamp);
    o.setProperty("lvl",    q.levelDb);
    o.setProperty("bell",   q.bellMode);
    o.setProperty("bellT",  q.bellTable);
    o.setProperty("vibC",   q.vibCents);
    o.setProperty("vibR",   q.vibRateHz);
    o.setProperty("vibO",   q.vibOnsetSec);
    o.setProperty("vibL",   q.vibLife);
    o.setProperty("drift",  q.driftCents);
    o.setProperty("bright", q.brightRamp);
    o.setProperty("uni",    q.unisonCents);
    o.setProperty("bodyHz", q.bodyHz);
    o.setProperty("bodyDb", q.bodyDb);
    o.setProperty("burst",  q.burstDb);
    o.setProperty("burstMs",q.burstMs);
    o.setProperty("burstT", q.burstTilt);
    o.setProperty("noise",  q.noiseDb);
    o.setProperty("noiseT", q.noiseTilt);
    o.setProperty("spectrum", q.spectrum);
    o.setProperty("cutoff", q.cutoffHz);
    o.setProperty("filterSweep", q.filterSweepOct);
    o.setProperty("filterDecay", q.filterDecayMs);
    o.setProperty("pitchAttack", q.pitchAttackCents);
    o.setProperty("pitchSettle", q.pitchSettleMs);
}

void decodeParams(const juce::DynamicObject& o, TimbreSlotParams& q)
{
    auto get = [&](const char* k, double d)
    { return o.hasProperty(k) ? (double) o.getProperty(k) : d; };

    q.preset        = (int) get("preset", q.preset);
    if (q.preset < kPresetCustom || q.preset >= numPresets()) q.preset = kPresetCustom;
    q.customBase    = juce::jlimit(-1, numPresets() - 1, (int) get("cbase", q.customBase));
    q.numPartials   = juce::jlimit(0, 128, (int) get("part", q.numPartials));
    q.slopeDbPerOct = get("slope",  q.slopeDbPerOct);
    q.oddBias       = get("odd",    q.oddBias);
    q.inharmonicity = get("inh",    q.inharmonicity);
    q.combDepth     = get("comb",   q.combDepth);
    q.combPos       = get("cpos",   q.combPos);
    q.attackMs      = get("atk",    q.attackMs);
    q.decaySec      = get("dec",    q.decaySec);
    q.hfDamp        = get("hf",     q.hfDamp);
    q.levelDb       = get("lvl",    q.levelDb);
    if (o.hasProperty("bell"))
        q.bellMode  = (bool) o.getProperty("bell");
    q.bellTable     = juce::jlimit(0, numTables() - 1, (int) get("bellT", q.bellTable));
    q.vibCents      = get("vibC",   q.vibCents);
    q.vibRateHz     = get("vibR",   q.vibRateHz);
    q.vibOnsetSec   = get("vibO",   q.vibOnsetSec);
    q.vibLife       = get("vibL",   q.vibLife);
    q.driftCents    = get("drift",  q.driftCents);
    q.brightRamp    = get("bright", q.brightRamp);
    q.unisonCents   = get("uni",    q.unisonCents);
    q.bodyHz        = get("bodyHz", q.bodyHz);
    q.bodyDb        = get("bodyDb", q.bodyDb);
    q.burstDb       = get("burst",  q.burstDb);
    q.burstMs       = get("burstMs",q.burstMs);
    q.burstTilt     = get("burstT", q.burstTilt);
    q.noiseDb       = get("noise",  q.noiseDb);
    q.noiseTilt     = get("noiseT", q.noiseTilt);
    // Missing new fields must load the legacy model, even when decoding over
    // a factory-preset default that now has a richer recipe.
    q.spectrum = juce::jlimit(0, (int) Spectrum::Count - 1, (int) get("spectrum", 0));
    q.cutoffHz = juce::jlimit(0.0, 20000.0, get("cutoff", 0.0));
    q.filterSweepOct = juce::jlimit(-4.0, 6.0, get("filterSweep", 0.0));
    q.filterDecayMs = juce::jlimit(5.0, 5000.0, get("filterDecay", 180.0));
    q.pitchAttackCents = juce::jlimit(-1200.0, 2400.0, get("pitchAttack", 0.0));
    q.pitchSettleMs = juce::jlimit(5.0, 1000.0, get("pitchSettle", 35.0));
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

namespace
{
    // Relative harmonic fingerprints, added to the editable spectral slope.
    // These are synthesis recipes, not measured recordings. The first twelve
    // harmonics carry the instrument identity; upper harmonics follow the tilt.
    constexpr double kFingerprint[][12] = {
        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { -2, 0, -1, -4, -2, -7, -5, -10, -8, -12, -10, -15 }, // piano
        { 0, -18, -3, -24, -12, -30, -16, -34, -22, -38, -28, -42 }, // tine
        { 0, -1, -4, -2, -7, -5, -12, -8, -15, -12, -18, -14 },
        { 0, 1, -2, -6, -5, -11, -9, -15, -13, -19, -17, -23 },
        { -1, 0, 1, -3, -2, -7, -4, -10, -7, -13, -10, -16 },
        { 0, -5, 1, -7, -1, -10, -3, -13, -6, -16, -9, -19 },
        { -4, 0, -2, -1, -5, -3, -8, -5, -10, -8, -12, -10 },
        { 0, -7, -15, -24, -29, -36, -40, -45, -48, -52, -55, -58 },
        { 0, -7, 0, -5, -2, -3, -5, -2, -8, -1, -11, 0 },
        { -8, 0, -1, 1, -2, -1, -5, -4, -8, -6, -11, -9 },
        { -6, -2, 0, -1, -3, -4, -6, -8, -10, -12, -14, -16 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, // vowels use fixed formants below
        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 0, -3, -9, -6, -48, -15, -48, -18, -22, -48, -48, -48 },
        { 0, -12, -2, -18, -6, -24, -10, -30, -16, -36, -22, -42 },
    };
    static_assert(std::size(kFingerprint) == (size_t) Spectrum::Count);

    double formantDb(int spectrum, double f)
    {
        const double* formants = nullptr;
        static const double ah[] = { 750.0, 1150.0, 2800.0 };
        static const double oo[] = { 320.0, 700.0, 2400.0 };
        static const double ee[] = { 300.0, 2200.0, 3000.0 };
        if (spectrum == (int) Spectrum::VoiceAh) formants = ah;
        if (spectrum == (int) Spectrum::VoiceOo) formants = oo;
        if (spectrum == (int) Spectrum::VoiceEe) formants = ee;
        if (!formants) return 0.0;
        double prominence = 0.0;
        for (int i = 0; i < 3; ++i)
        {
            const double d = std::log2(f / formants[i]) / (i == 0 ? 0.32 : 0.22);
            prominence = juce::jmax(prominence, (22.0 - 3.0 * i) * std::exp(-0.5 * d * d));
        }
        return prominence - 22.0;
    }
}

std::vector<Partial> computePartials(const TimbreSlotParams& p)
{
    std::vector<Partial> out;
    const double f0 = midiNoteHz(p.midiNote);

    if (p.bellMode)
    {
        const TableDef& t = kTables[juce::jlimit(0, numTables() - 1, p.bellTable)];
        out.reserve((size_t) t.n);
        for (int i = 0; i < t.n; ++i)
        {
            const double damping = 1.0 + juce::jlimit(0.0, 1.0, p.hfDamp) * 0.4
                * juce::jmax(0.0, std::log2(t.rows[i].ratio));
            out.push_back({ f0 * t.rows[i].ratio, t.rows[i].ampDb, t.rows[i].decayMul * damping });
        }
        return out;
    }

    const int N = juce::jlimit(0, 128, p.numPartials);
    out.reserve((size_t) N);
    for (int n = 1; n <= N; ++n)
    {
        // Stiff-string stretch: f_n = n·f0·√(1 + B·n²)
        const double stretch = std::sqrt((1.0 + juce::jmax(0.0, p.inharmonicity) * (double) n * n)
                                       / (1.0 + juce::jmax(0.0, p.inharmonicity)));
        const double f = f0 * n * stretch;

        double db = p.slopeDbPerOct * std::log2((double) n);
        const int shape = juce::jlimit(0, (int) Spectrum::Count - 1, p.spectrum);
        // Extend the final harmonic offset into the upper rolloff; no step at 13.
        db += kFingerprint[shape][juce::jmin(n, 12) - 1];
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
// Voice model
//==============================================================================
VoiceModel buildVoiceModel(const TimbreSlotParams& p)
{
    VoiceModel vm;
    vm.enabled = p.enabled;
    if (! p.enabled)
        return vm;

    TimbreSlotParams ref = p;
    ref.midiNote = 69;                                // A4 reference — ratios only
    vm.rel   = computePartials(ref);
    vm.refHz = midiNoteHz(69);
    vm.maxAmpDb = -1.0e9;
    for (const auto& pt : vm.rel)
        vm.maxAmpDb = juce::jmax(vm.maxAmpDb, pt.ampDb);

    if (p.bodyHz > 0.0) vm.maxAmpDb += juce::jmax(0.0, p.bodyDb); // preserve resonance headroom
    vm.spectrum = p.bellMode ? (int) Spectrum::Neutral
                            : juce::jlimit(0, (int) Spectrum::Count - 1, p.spectrum);
    vm.cutoffHz = juce::jlimit(0.0, 20000.0, p.cutoffHz);
    vm.filterSweepOct = juce::jlimit(-4.0, 6.0, p.filterSweepOct);
    vm.filterDecaySec = juce::jlimit(0.005, 5.0, p.filterDecayMs * 0.001);
    vm.pitchAttackCents = juce::jlimit(-1200.0, 2400.0, p.pitchAttackCents);
    vm.pitchSettleSec = juce::jlimit(0.005, 1.0, p.pitchSettleMs * 0.001);
    vm.attackSec   = juce::jmax(1.0e-4, p.attackMs / 1000.0);
    vm.decaySec    = p.decaySec;
    vm.levelDb     = p.levelDb;
    vm.brightRamp  = juce::jlimit(-1.0, 1.0, p.brightRamp);
    vm.vibCents    = juce::jmax(0.0, p.vibCents);
    vm.vibRateHz   = juce::jlimit(0.1, 20.0, p.vibRateHz);
    vm.vibOnsetSec = juce::jmax(0.0, p.vibOnsetSec);
    vm.vibLife     = juce::jlimit(0.0, 1.0, p.vibLife);
    vm.driftCents  = juce::jmax(0.0, p.driftCents);
    vm.unisonCents = juce::jmax(0.0, p.unisonCents);
    vm.bodyHz      = juce::jmax(0.0, p.bodyHz);
    vm.bodyDb      = p.bodyDb;
    vm.burstDb     = p.burstDb;
    vm.burstSec    = juce::jlimit(0.0005, 0.2, p.burstMs / 1000.0);
    vm.burstTilt   = p.burstTilt;
    vm.noiseDb     = p.noiseDb;
    vm.noiseTilt   = p.noiseTilt;

    // A texture is measured against "the strongest partial = 0 dB"; a sound
    // with no partial stack (hi-hat) keeps that reference so its noise sits
    // where the slider says.
    if (vm.rel.empty() && (vm.hasBurst() || vm.hasNoise()))
        vm.maxAmpDb = 0.0;
    return vm;
}

//==============================================================================
// Note painter
//==============================================================================
namespace
{
    inline double mmToPx(double mm, double dpi) { return mm * dpi / 25.4; }

    // Grain cell of the noise textures, in PHYSICAL units so the same cell
    // lands on the same spot at every resolution (150 DPI preview, 400 DPI
    // page, the playback strip): the grain is a property of the sound.
    constexpr double kGrainSec   = 0.004;   // 4 ms
    constexpr double kGrainSemis = 0.20;    // a fifth of a semitone

    // Deterministic hash → uniform [0,1). No global RNG anywhere: a re-render
    // is bit-identical, and a note keeps its grain across a page boundary.
    inline juce::uint32 hash3(juce::uint32 a, juce::uint32 b, juce::uint32 c)
    {
        juce::uint32 h = a * 0x9E3779B1u;
        h ^= (b + 0x7F4A7C15u) * 0x85EBCA77u;
        h ^= (c + 0x165667B1u) * 0xC2B2AE3Du;
        h ^= h >> 15; h *= 0x2C1B3C6Du;
        h ^= h >> 12; h *= 0x297A2D39u;
        h ^= h >> 15;
        return h;
    }
    inline double unit(juce::uint32 h) { return (double) (h >> 8) * (1.0 / 16777216.0); }

    inline double smoothRise(double x)
    {
        x = juce::jlimit(0.0, 1.0, x);
        return x * x * (3.0 - 2.0 * x);
    }

    // Unit-mean exponential power (Rayleigh amplitude). Interpolate POWER
    // between cells with a smoothstep; independent cells no longer jump every
    // 4 ms. The seed is note-relative, so clipped/page-split renders agree.
    inline double grainPower(juce::uint32 t, juce::uint32 f, juce::uint32 seed)
    {
        const double u = juce::jlimit(1.0e-6, 1.0 - 1.0e-6, unit(hash3(t, f, seed)));
        return -std::log(1.0 - u);
    }

    inline double filterDb(const VoiceModel& vp, double f, double tau)
    {
        if (vp.cutoffHz <= 0.0) return 0.0;
        const double cutoff = vp.cutoffHz * std::exp2(vp.filterSweepOct
            * std::exp(-juce::jmax(0.0, tau) / vp.filterDecaySec));
        const double ratio = f / juce::jmax(10.0, cutoff);
        const double r2 = ratio * ratio, r4 = r2 * r2;
        return -10.0 * std::log10(1.0 + r4 * r4);
    }

}

struct NotePainter::Impl
{
    Impl(juce::Image& img, const BandGeom& geom, const InkSettings& inkIn)
        : bmp(img, juce::Image::BitmapData::readWrite),
          imageW(img.getWidth()), g(geom), ink(inkIn)
    {
        range     = juce::jmax(1.0, ink.dynamicRangeDB);
        logRatio  = std::log(ink.maxFreq / ink.minFreq);
        // One partial = one reference reader bin's POWER, regardless of
        // line width or export DPI. 219.456 mm at 400 DPI is 3456 bins.
        referencePower = g.heightPx / mmToPx(SCORE_CIS_HEIGHT_MM, 400.0);
        minWidth = juce::jmax(0.35, referencePower);
        halfWidth = juce::jmax(minWidth, mmToPx(ink.lineWidthMM, ink.dpiY) * 0.5);
        dyMax     = (int) std::ceil(halfWidth * 2.0) + 1;   // Gaussian skirt
        // On the LOG axis a pitch offset in cents is the same px offset at
        // any frequency — one conversion serves every wave below.
        pxPerCent = (std::log(2.0) / 1200.0) / logRatio * g.heightPx;
        rowsPerOct = g.heightPx / (logRatio / std::log(2.0));
    }

    juce::Image::BitmapData bmp;
    const int       imageW;
    const BandGeom  g;
    const InkSettings ink;
    double range, logRatio, halfWidth, minWidth, referencePower, pxPerCent, rowsPerOct;
    int    dyMax;
    std::vector<double> yOff, amDb;     // per-column, reused across notes

    //--------------------------------------------------------------------------
    inline void darken(int x, int y, double intensity)   // 0 = black … 1 = white
    {
        if (x < 0 || x >= imageW || y < g.yTop || y >= g.yBot) return;
        const auto v = (juce::uint8) juce::jlimit(0, 255, (int) (intensity * 255.0 + 0.5));
        juce::uint8* p = bmp.getLinePointer(y) + x * bmp.pixelStride;
        if (v < p[0]) p[0] = p[1] = p[2] = v;            // darker (louder) wins
    }

    // Panned ink, SCORE stereo convention: R byte = RIGHT brightness,
    // B byte = LEFT brightness, G = the darker of the two — left-only ink
    // is red, right-only blue, centre grey (byte-identical to mono).
    // Darker-wins per CHANNEL so overlapping notes with different pans
    // composite exactly like ScoreGen's stereo cells.
    inline void darkenLR(int x, int y, double intL, double intR)
    {
        if (x < 0 || x >= imageW || y < g.yTop || y >= g.yBot) return;
        const auto vL = (juce::uint8) juce::jlimit(0, 255, (int) (intL * 255.0 + 0.5));
        const auto vR = (juce::uint8) juce::jlimit(0, 255, (int) (intR * 255.0 + 0.5));
        auto* p = reinterpret_cast<juce::PixelRGB*>(bmp.getLinePointer(y) + x * bmp.pixelStride);
        p->setARGB(255,
                   juce::jmin(p->getRed(),   vR),
                   juce::jmin(p->getGreen(), juce::jmin(vL, vR)),
                   juce::jmin(p->getBlue(),  vL));
    }

    /** One cell of ink at (x, y) at `dB` (≤ 0 = printable), panned or not. */
    inline void ink1(int x, int y, double dB, const double* pL, const double* pR)
    {
        if (pL == nullptr)
        {
            const double v = juce::jlimit(0.0, 1.0, -dB / range);
            if (v < 1.0) darken(x, y, v);
        }
        else
        {
            const double vL = juce::jlimit(0.0, 1.0, -(dB + pL[x - g.xMin]) / range);
            const double vR = juce::jlimit(0.0, 1.0, -(dB + pR[x - g.xMin]) / range);
            if (vL < 1.0 || vR < 1.0) darkenLR(x, y, vL, vR);
        }
    }

    inline double colTime(int x) const { return ((x + 0.5) - g.x0Px) / g.pxPerSec + g.t0; }
    inline double rowHz(double y) const
    { return ink.minFreq * std::exp((g.yBottom - y) / g.heightPx * logRatio); }
    inline double hzRow(double f) const
    { return g.yBottom - std::log(f / ink.minFreq) / logRatio * g.heightPx; }

    //--------------------------------------------------------------------------
    /** Living pitch wave of ONE note, sampled per pixel column: vibrato,
     *  drift, and the captured curves of an MPE take, all folded into two
     *  per-column arrays (pitch offset in px, coupled level in dB).
     *
     *  The whole partial stack is frequency-modulated by the same ±cents
     *  wave; on the LOG axis that is a single vertical offset shared by every
     *  partial, so it is computed once per note.
     *
     *  Everything is deterministic in tau (a note crossing a page boundary
     *  keeps its wave phase, and playback matches the print):
     *   - vibrato onset: nothing at the attack, then the depth develops
     *     smoothly over vibOnsetSec (delay + smoothstep rise), eases out at
     *     the tail; waving depth; drifting rate (integrated analytically);
     *     per-note defects drawn from a hash of the note's index;
     *   - drift: three incommensurate slow sines, alive from the first
     *     column — a partial is never a ruler.
     *  vibLife scales every vibrato irregularity: 0 = metronomic, 1 = loose. */
    bool computeWave(const VoiceModel& vp, const NoteInk& n, int x0, int x1)
    {
        const size_t nCols = (size_t) juce::jmax(0, x1 - x0);
        yOff.assign(nCols, 0.0);
        amDb.assign(nCols, 0.0);
        if (nCols == 0 || pxPerCent <= 0.0)
            return false;

        const bool hasVib   = vp.vibCents > 0.05;
        const bool hasDrift = vp.driftCents > 0.05;
        const bool hasBend  = n.bendPts  != nullptr && ! n.bendPts->empty();
        const bool hasLvl   = n.levelPts != nullptr && ! n.levelPts->empty();
        const bool hasPitchAttack = std::abs(vp.pitchAttackCents) > 0.01;
        if (! hasVib && ! hasDrift && ! hasBend && ! hasLvl && ! hasPitchAttack)
            return false;

        const double twoPi = juce::MathConstants<double>::twoPi;
        const double dur   = n.endSec - n.startSec;

        // Deterministic per-note randoms.
        juce::uint32 h = (juce::uint32) (n.noteIndex + 1) * 2654435761u
                       ^ (juce::uint32) ((int) std::lround(n.f0Hz * 10.0) * 40503 + 977);
        auto rnd = [&h]() { h = h * 1664525u + 1013904223u; return unit(h); };

        // Vibrato ingredients.
        const double life     = vp.vibLife;
        const double phase0   = rnd() * twoPi * life;
        const double rateMul  = 1.0 + (rnd() - 0.5) * 0.16 * life;   // ±8 %
        const double depthMul = 1.0 + (rnd() - 0.5) * 0.50 * life;   // ±25 %
        const double driftPh  = rnd() * twoPi;
        const double undPh    = rnd() * twoPi;
        const double undHz    = 0.8 * (0.6 + 0.8 * rnd());           // 0.48..1.12 Hz
        const double delay    = vp.vibOnsetSec * 0.4;
        const double rise     = juce::jmax(0.08, vp.vibOnsetSec * 0.6);
        const double easeSec  = juce::jmin(0.25, dur * 0.25);        // tail ease-out
        const double rate     = vp.vibRateHz * rateMul;
        const double wobW     = twoPi * 0.9;                         // rate-wobble speed
        const double wobA     = 0.10 * life;                         // ±10 % rate wobble

        // Drift ingredients: three slow sines whose phases differ per note.
        const double dPh1 = rnd() * twoPi, dPh2 = rnd() * twoPi, dPh3 = rnd() * twoPi;

        const double baseDb = velocityDb(n.velocity, n.velocityRangeDb);
        size_t ib = 0, il = 0;

        for (size_t i = 0; i < nCols; ++i)
        {
            const double tau = colTime(x0 + (int) i) - n.startSec;
            if (tau < 0.0 || tau > dur)
                continue;

            if (hasPitchAttack)
                yOff[i] += vp.pitchAttackCents * std::exp(-tau / vp.pitchSettleSec) * pxPerCent;
            if (hasVib)
            {
                double env = 0.0;                                    // onset smoothstep
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
                if (env > 0.0)
                {
                    // Depth undulation: comes and goes between 100 % and (1 − 0.35·life).
                    const double und = 1.0 - 0.35 * life
                                     * (0.5 + 0.5 * std::sin(twoPi * undHz * tau + undPh));
                    // Phase with drifting rate, integrated analytically:
                    // rate(t) = R(1 + a·sin(wt+p))  →  ∫ = R·tau − (R·a/w)(cos(wtau+p) − cos p)
                    const double phase = twoPi * rate * tau
                                       + (twoPi * rate * wobA / wobW)
                                         * (std::cos(driftPh) - std::cos(wobW * tau + driftPh))
                                       + phase0;
                    const double amp = vp.vibCents * depthMul * env * und;   // cents, now
                    yOff[i] += amp * std::sin(phase) * pxPerCent;
                    // Subtle coupled AM (never above the note's own level): a
                    // breath of ~0.6 dB per 30 cents, in quadrature with the pitch.
                    const double amDepth = 0.018 * amp;
                    amDb[i] += amDepth * 0.5
                             * (std::sin(phase - juce::MathConstants<double>::halfPi) - 1.0);
                }
            }

            if (hasDrift)
            {
                const double w = 0.55 * std::sin(twoPi * 0.31 * tau + dPh1)
                               + 0.30 * std::sin(twoPi * 0.79 * tau + dPh2)
                               + 0.15 * std::sin(twoPi * 1.63 * tau + dPh3);
                yOff[i] += vp.driftCents * w * pxPerCent;
            }

            // Captured curves: piecewise-constant walk, breakpoints sorted
            // and tau growing with the column.
            if (hasBend)
            {
                const auto& pts = *n.bendPts;
                while (ib + 1 < pts.size() && pts[ib + 1].first <= tau) ++ib;
                yOff[i] += (double) pts[ib].second * pxPerCent;
            }
            if (hasLvl)
            {
                const auto& pts = *n.levelPts;
                while (il + 1 < pts.size() && pts[il + 1].first <= tau) ++il;
                amDb[i] += velocityDb((int) pts[il].second, n.velocityRangeDb) - baseDb;
            }
        }
        return true;
    }

    /** Vertical reach of the wave arrays, for the row cull. */
    int waveMarginPx(const VoiceModel& vp, const NoteInk& n) const
    {
        double cents = vp.vibCents * 1.25 + vp.driftCents;
        if (n.bendPts != nullptr)
            for (const auto& bp : *n.bendPts)
                cents = juce::jmax(cents, std::abs((double) bp.second) + vp.driftCents);
        cents += std::abs(vp.pitchAttackCents);
        return (int) std::ceil(cents * pxPerCent) + 1;
    }

    //--------------------------------------------------------------------------
    /** Draws one soft-edged partial line across [x0..x1): attack (with its
     *  own attack time), decay, tail fade, the shared wave, the pan. */
    void paintLine(double fHz, double baseDb, double attackSec, double decayMul,
                   const NoteInk& n, double dur, double fadeSec, int x0, int x1,
                   bool hasWave, const VoiceModel& vp)
    {
        const double yC  = hzRow(fHz);
        const int    yCi = (int) std::round(yC);
        const int margin = hasWave ? waveMarginPx(vp, n) : 0;
        if (yCi + dyMax + margin < g.yTop || yCi - dyMax - margin >= g.yBot)
            return;

        // Upper harmonics occupy less log-frequency width, limiting the
        // beating cloud around high notes. The attack opens into the requested
        // width gradually. Power is conserved during this opening.
        const double harmonic = juce::jmax(1.0, fHz / n.f0Hz);
        const double settledWidth = juce::jmax(minWidth, halfWidth / std::sqrt(harmonic));
        const double widthRise = juce::jmax(0.012, attackSec * 2.0);
        std::vector<double> profileDb((size_t)(2 * dyMax + 1));
        double previousWidth = -1.0, previousFraction = -10.0;

        for (int x = x0; x < x1; ++x)
        {
            // Column time relative to the NOTE start (absolute — so a note
            // crossing a page boundary keeps its envelope phase).
            const double tau = colTime(x) - n.startSec;
            if (tau < 0.0 || tau > dur)
                continue;

            double envDb = 0.0;
            if (tau < attackSec)                                   // attack ramp
                envDb += 20.0 * std::log10(juce::jmax(smoothRise(tau / attackSec), 1.0e-4));
            if (vp.decaySec > 0.0 && tau > attackSec)              // −60 dB decay
                envDb += -60.0 * (tau - attackSec) / vp.decaySec * decayMul;
            const double tail = dur - tau;
            if (tail < fadeSec)
                envDb += 20.0 * std::log10(juce::jmax(smoothRise(tail / fadeSec), 1.0e-4));

            double yCol = yC;
            if (hasWave)
            {
                yCol -= yOff[(size_t) (x - x0)];                   // +cents = higher = up
                envDb += amDb[(size_t) (x - x0)];
            }

            envDb += filterDb(vp, fHz, tau);
            const double dB = baseDb + envDb;
            if (dB <= -range)
                continue;

            const int yCiCol = (int) std::round(yCol);
            const double width = juce::jmax(minWidth, settledWidth
                * (0.65 + 0.35 * smoothRise(tau / widthRise)));
            const double fraction = yCol - yCiCol;
            if (width != previousWidth || fraction != previousFraction)
            {
                // Integrate Gaussian POWER over each pixel cell, rather than
                // sample its peak. This avoids level wobble as a narrow line
                // crosses pixel centres, and prevents width from adding gain.
                const double k = std::sqrt(1.2 * std::log(10.0)) / width;
                for (int dy = -dyMax; dy <= dyMax; ++dy)
                {
                    const double lo = (dy - fraction - 0.5) * k;
                    const double hi = (dy - fraction + 0.5) * k;
                    const double power = referencePower * 0.5 * (std::erf(hi) - std::erf(lo));
                    profileDb[(size_t)(dy + dyMax)] = 10.0 * std::log10(juce::jmax(1.0e-20, power));
                }
                previousWidth = width; previousFraction = fraction;
            }
            for (int dy = -dyMax; dy <= dyMax; ++dy)
                ink1(x, yCiCol + dy, dB + profileDb[(size_t)(dy + dyMax)], n.panDbL, n.panDbR);

        }
    }

    //--------------------------------------------------------------------------
    /** A texture's dB control denotes TOTAL noise power relative to a
     * partial, not the level of each of hundreds of simultaneously sounding
     * bands. Normalise the spectral shape before applying time envelopes.
     * Texture below the printable floor stays silent instead of being raised.
     * Each column samples its true envelope time, never a grain midpoint. */
    template <typename LevelFn, typename EnvFn>
    void paintGrain(const NoteInk& n, double baseDb, double tA, double tB,
                    LevelFn levelAt, EnvFn envAt, juce::uint32 seed)
    {
        const double rowsPerCell = rowsPerOct * (kGrainSemis / 12.0);
        const int cellsF = (int) std::ceil(g.heightPx / rowsPerCell) + 1;
        const int xLo = juce::jmax(g.xMin, (int) std::floor(g.x0Px + (tA - g.t0) * g.pxPerSec));
        const int xHi = juce::jmin(g.xMax, (int) std::ceil(g.x0Px + (tB - g.t0) * g.pxPerSec));
        if (xHi <= xLo) return;

        struct Cell { int yA, yB; double f, shape; };
        std::vector<Cell> cells;
        cells.reserve((size_t)cellsF);
        double spectralPower = 0.0;
        for (int cf = 0; cf < cellsF; ++cf)
        {
            const int yB = juce::jmin(g.yBot, (int) std::lround(g.yBottom - cf * rowsPerCell));
            const int yA = juce::jmax(g.yTop, (int) std::lround(g.yBottom - (cf + 1) * rowsPerCell));
            const double f = rowHz(g.yBottom - (cf + 0.5) * rowsPerCell);
            const double shape = levelAt(f);
            cells.push_back({ yA, yB, f, shape });
            if (yB > yA) spectralPower += (yB - yA) * std::pow(10.0, shape / 10.0);
        }
        if (spectralPower <= 0.0) return;
        const double normalDb = 10.0 * std::log10(referencePower / spectralPower);
        for (size_t cf = 0; cf < cells.size(); ++cf)
        {
            const auto& c = cells[cf];
            if (c.yB <= c.yA) continue;
            const double level = baseDb + c.shape + normalDb;
            // Rayleigh power is bounded by the hash's 1e-6 tail (~11.4 dB).
            if (level + 12.0 <= -range) continue;
            long cachedCell = -1;
            double a = 0.0, b = 0.0;
            for (int x = xLo; x < xHi; ++x)
            {
                const double tau = colTime(x) - n.startSec;
                if (tau < 0.0 || colTime(x) >= tB) continue;
                const double pos = tau / kGrainSec;
                const long ct = (long) std::floor(pos);
                if (ct != cachedCell)
                {
                    a = grainPower((juce::uint32)ct, (juce::uint32)cf, seed);
                    b = grainPower((juce::uint32)(ct + 1), (juce::uint32)cf, seed);
                    cachedCell = ct;
                }
                const double weight = smoothRise(pos - ct);
                const double dB = level + envAt(tau, c.f)
                    + 10.0 * std::log10(juce::jmax(1.0e-12, a + (b - a) * weight));
                if (dB <= -range) continue;
                for (int y = c.yA; y < c.yB; ++y) ink1(x, y, dB, n.panDbL, n.panDbR);
            }
        }
    }

};

NotePainter::NotePainter(juce::Image& img, const BandGeom& g, const InkSettings& ink)
    : impl_(new Impl(img, g, ink)) {}

NotePainter::~NotePainter() { delete impl_; }

void NotePainter::paint(const VoiceModel& vp, const NoteInk& n)
{
    Impl& I = *impl_;
    if (! vp.hasInk())
        return;
    if (n.endSec <= I.g.t0 || n.startSec >= I.g.t1)
        return;   // outside this window

    const double baseAll = n.levelDb - I.ink.maxDb;                 // ≤ 0
    if (baseAll <= -I.range)
        return;
    if (n.panDbL != nullptr && n.panDbR != nullptr)
        stereoInk_ = true;

    const double dur     = n.endSec - n.startSec;
    const double fadeSec = n.fadeSec > 0.0 ? n.fadeSec : juce::jmin(0.04, dur * 0.2);

    const double tA = juce::jmax(n.startSec, I.g.t0);
    const double tB = juce::jmin(n.endSec,   I.g.t1);
    const int x0 = juce::jmax(I.g.xMin, (int) std::floor(I.g.x0Px + (tA - I.g.t0) * I.g.pxPerSec));
    const int x1 = juce::jmin(I.g.xMax, (int) std::ceil (I.g.x0Px + (tB - I.g.t0) * I.g.pxPerSec));

    // ── The partial stack ────────────────────────────────────────────────────
    if (! vp.rel.empty() && x1 > x0)
    {
        const bool hasWave = I.computeWave(vp, n, x0, x1);
        const bool unison  = vp.unisonCents > 0.5;
        const double uniRatio = std::pow(2.0, vp.unisonCents / 2400.0);
        const bool body    = vp.bodyHz > 0.0 && std::abs(vp.bodyDb) > 0.01;

        int idx = 0;
        for (const auto& pt : vp.rel)
        {
            ++idx;
            const double f = n.f0Hz * (pt.freqHz / vp.refHz);
            double baseDb = baseAll + pt.ampDb + formantDb(vp.spectrum, f);
            // A body resonance and a voice EQ act at the partial's ABSOLUTE
            // frequency — absolute gains, never folded into the normalisation.
            if (body)
            {
                const double d = std::log2(f / vp.bodyHz) / 0.5;      // σ = half an octave
                baseDb += vp.bodyDb * std::exp(-0.5 * d * d);
            }
            if (n.gainDbAt != nullptr)
                baseDb += (*n.gainDbAt)(f);
            if (baseDb <= -I.range)
                continue;

            // Brightness ramp: partial n takes attackSec × n^ramp to arrive.
            const double attackSec = vp.attackSec
                                   * std::pow((double) idx, vp.brightRamp);

            if (! unison)
            {
                if (f >= I.ink.minFreq && f <= I.ink.maxFreq)
                    I.paintLine(f, baseDb, attackSec, pt.decayMul, n, dur, fadeSec,
                                x0, x1, hasWave, vp);
            }
            else
            {
                // A detuned pair, each −3 dB: the two oscillators the reader
                // will play a few cents apart beat against each other for real.
                for (const double fu : { f / uniRatio, f * uniRatio })
                    if (fu >= I.ink.minFreq && fu <= I.ink.maxFreq)
                        I.paintLine(fu, baseDb - 3.0, attackSec, pt.decayMul, n, dur,
                                    fadeSec, x0, x1, hasWave, vp);
            }
        }
    }

    const juce::uint32 seed = (juce::uint32) (n.noteIndex * 7919u + 13u);

    // ── Sustained noise: breath, bow, wind ───────────────────────────────────
    // Texture levels reference this voice's strongest partial, so normalising
    // a quiet spectral recipe must not amplify its background noise.
    // Tied to the fundamental (it moves with the note, like an excitation
    // coupled to its resonator) and to the note's own envelope.
    if (vp.hasNoise() && tB > tA)
    {
        const double f0 = n.f0Hz;
        auto levelAt = [&](double f) -> double
        {
            const double lift = juce::jmax(0.0, vp.noiseTilt * std::log2(juce::jmax(f0, I.ink.maxFreq) / f0));
            return (f >= f0 ? vp.noiseTilt * std::log2(f / f0) - lift
                            : -9.0 * std::log2(f0 / f) - lift) + formantDb(vp.spectrum, f);      // rolls off under f0
        };
        auto envAt = [&](double tau, double f) -> double
        {
            if (tau < 0.0 || tau > dur) return -1.0e9;
            double e = filterDb(vp, f, tau);
            if (tau < vp.attackSec)
                e += 20.0 * std::log10(juce::jmax(smoothRise(tau / vp.attackSec), 1.0e-4));
            if (vp.decaySec > 0.0 && tau > vp.attackSec)
                e += -60.0 * (tau - vp.attackSec) / vp.decaySec;
            const double tail = dur - tau;
            if (tail < fadeSec)
                e += 20.0 * std::log10(juce::jmax(smoothRise(tail / fadeSec), 1.0e-4));
            return e;
        };
        I.paintGrain(n, baseAll + vp.maxAmpDb + vp.noiseDb, tA, tB, levelAt, envAt, seed);
    }

    // ── Onset burst: pick, hammer, tongue, bow grip ──────────────────────────
    // A half-Gaussian grain starting AT the onset whose width shrinks with
    // frequency (∝ 1/√f): the way a click spreads on a constant-Q image, and
    // the way it must be printed for the reader to play a chuff of the right
    // colour rather than a smear. Independent of the partials' envelope — it
    // is the excitation, not the resonance — but scaled by the note's level.
    if (vp.hasBurst())
    {
        const double wMax = vp.burstSec * 4.0;
        const double bA = juce::jmax(n.startSec, I.g.t0);
        const double bB = juce::jmin(n.startSec + 3.0 * wMax, juce::jmin(n.endSec, I.g.t1));
        if (bB > bA)
        {
            auto levelAt = [&](double f) -> double
            {
                const double edge = vp.burstTilt >= 0.0 ? I.ink.maxFreq : I.ink.minFreq;
                const double lift = juce::jmax(0.0, vp.burstTilt * std::log2(edge / 1000.0));
                return vp.burstTilt * std::log2(f / 1000.0) - lift;
            };
            auto envAt = [&](double tau, double f) -> double
            {
                if (tau < 0.0) return -1.0e9;
                const double w = juce::jlimit(0.25, 4.0, std::sqrt(1000.0 / f)) * vp.burstSec;
                const double a = tau / w;
                const double rise = juce::jlimit(0.001, 0.004, w * 0.35);
                return 20.0 * std::log10(juce::jmax(smoothRise(tau / rise), 1.0e-4))
                     - 8.686 * a * a + filterDb(vp, f, tau);
            };
            I.paintGrain(n, baseAll + vp.maxAmpDb + vp.burstDb, bA, bB, levelAt, envAt, seed ^ 0x5bd1e995u);
        }
    }
}

//==============================================================================
// Page rendering
//==============================================================================
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
    const double gapSec   = gapPx / pxPerSec;

    // ── Page-wide normalisation: strongest partial of any enabled slot = 0 dB ─
    // (envelope peaks at 0, so the loudest printed cell hits full black exactly
    // like SCORE's global_max normalisation).
    std::array<VoiceModel, kNumSlots> models;
    double pageMaxDb = -1.0e9;
    for (int i = 0; i < kNumSlots; ++i)
    {
        models[(size_t) i] = buildVoiceModel(slots[(size_t) i]);
        const auto& vm = models[(size_t) i];
        if (vm.hasInk())
            pageMaxDb = juce::jmax(pageMaxDb, vm.maxAmpDb + vm.levelDb);
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

    // ── Draw each enabled slot as ONE note on the page's timeline ────────────
    {
        BandGeom geom;
        geom.x0Px = spectroLeft;
        geom.xMin = (int) std::floor(spectroLeft);
        geom.xMax = juce::jmin(imageW, (int) std::ceil(spectroLeft + spectroWidth));
        geom.yBottom = spectroBottom;  geom.heightPx = spectroHeightPx;
        geom.yTop = yTop;  geom.yBot = yBot;
        geom.pxPerSec = pxPerSec;  geom.t0 = 0.0;  geom.t1 = spectroWidth / pxPerSec;

        InkSettings ink;
        ink.minFreq = settings.minFreq;   ink.maxFreq = settings.maxFreq;
        ink.dynamicRangeDB = settings.dynamicRangeDB;
        ink.lineWidthMM = settings.lineWidthMM;
        ink.dpiY  = dpi;
        ink.maxDb = pageMaxDb;

        // End fade: 1.5 mm of paper or 10 % of the slot, whichever is shorter
        // — anti-click, and a visible separation between the strips.
        const double fadeSec = juce::jmin(0.15 / settings.writingSpeed, slotSec * 0.10);

        NotePainter painter(img, geom, ink);
        for (int si = 0; si < kNumSlots; ++si)
        {
            const auto& vm = models[(size_t) si];
            if (! vm.hasInk()) continue;
            NoteInk note;
            note.f0Hz      = midiNoteHz(slots[(size_t) si].midiNote);
            note.startSec  = si * (slotSec + gapSec);
            note.endSec    = note.startSec + slotSec;
            note.levelDb   = vm.levelDb;
            note.noteIndex = (size_t) si;
            note.fadeSec   = fadeSec;
            painter.paint(vm, note);
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
    juce::StringArray logLines;
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
