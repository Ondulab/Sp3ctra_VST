#include "image/TimbreGenRenderer.h"
#include "image/TimbreParamsPanel.h"
#include "Sp3ctraLookAndFeel.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <set>
using namespace timbregen;
void testNoiseRendering(const juce::File&);

static juce::ComboBox& combo(TimbreParamsPanel& panel, const char* id)
{
    auto* c = dynamic_cast<juce::ComboBox*>(panel.findChildWithID(id));
    assert(c); return *c;
}
static void choose(juce::ComboBox& c, const char* text)
{
    for (int i = 0; i < c.getNumItems(); ++i)
        if (c.getItemText(i) == text) { c.setSelectedId(c.getItemId(i), juce::sendNotificationSync); return; }
    assert(false && "missing browser item");
}
static juce::Image render(const TimbreSlotParams& p)
{
    juce::Image img(juce::Image::RGB, 320, 512, true);
    { juce::Graphics g(img); g.fillAll(juce::Colours::white); }
    BandGeom band;
    band.xMax = 320; band.yBottom = 512; band.heightPx = 512; band.yBot = 512;
    band.pxPerSec = 128; band.t1 = 2.5;
    InkSettings ink;
    ink.minFreq = 30; ink.maxFreq = 16000; ink.dpiY = 100; ink.lineWidthMM = .25;
    const auto vm = buildVoiceModel(p); ink.maxDb = vm.maxAmpDb;
    NoteInk note;
    note.f0Hz = midiNoteHz(p.midiNote); note.endSec = 2.5; note.noteIndex = 37;
    NotePainter painter(img, band, ink); painter.paint(vm, note);
    return img;
}
static uint64_t fingerprint(const juce::Image& img)
{
    uint64_t h = 1469598103934665603ull;
    for (int y = 0; y < img.getHeight(); ++y)
        for (int x = 0; x < img.getWidth(); ++x)
            h = (h ^ img.getPixelAt(x,y).getRed()) * 1099511628211ull;
    return h;
}
static double centroid(const juce::Image& img, int x)
{
    double power = 0, weighted = 0;
    for (int y=0; y<img.getHeight(); ++y)
    {
        const int pixel = img.getPixelAt(x,y).getRed();
        if (pixel == 255) continue;
        const double energy = std::pow(10.0, -(pixel / 255.0) * 50.0 / 10.0);
        const double hz = 30.0 * std::pow(16000.0 / 30.0, (512.0-y) / 512.0);
        power += energy; weighted += energy * hz;
    }
    assert(power > 0); return weighted / power;
}
static void writePng(const juce::Image& img, const juce::File& file)
{
    auto out = file.createOutputStream(); assert(out);
    out->setPosition(0); out->truncate();
    assert(juce::PNGImageFormat().writeImageToStream(img, *out));
}
int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    Sp3ctraLookAndFeel look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    const juce::File dest(argc > 1 ? argv[1] : "/tmp/sp3ctra-timbres"); dest.createDirectory();
    std::set<juce::String> families, names;
    int basses = 0;
    std::set<uint64_t> images;
    juce::Image gallery(juce::Image::RGB, 8*200, ((numPresets()+7)/8)*145, true);
    juce::Graphics galleryG(gallery); galleryG.fillAll(juce::Colour(0xff191b20));
    for (int i=0; i<numPresets(); ++i)
    {
        TimbreSlotParams p;
        p.enabled = false; p.midiNote = 43; p.levelDb = -7;
        p.customBase = 2;
        applyPreset(p,i);
        assert(p.preset == i && p.customBase == -1);
        assert(!p.enabled && p.midiNote == 43 && p.levelDb == -7);
        assert(names.insert(presetName(i)).second);
        assert(juce::String(presetSubfamily(i)).isNotEmpty());
        families.insert(presetFamily(i));
        if (juce::String(presetFamily(i)) == "Bass") ++basses;
        p.enabled = true; p.levelDb = 0;
        for (const int note : { 28, 48, 69, 96 })
        {
            p.midiNote = note;
            for (auto pt : computePartials(p))
                assert(std::isfinite(pt.freqHz) && pt.freqHz > 0 && std::isfinite(pt.ampDb)
                       && std::isfinite(pt.decayMul) && pt.decayMul >= 0);
        }
        p.midiNote = presetSuggestedNote(i);
        juce::DynamicObject encoded; encodeParams(encoded,p);
        TimbreSlotParams decoded; decodeParams(encoded,decoded);
        juce::DynamicObject reencoded; encodeParams(reencoded,decoded);
        const auto& props = encoded.getProperties();
        for (int k=0; k<props.size(); ++k)
            assert(props.getValueAt(k) == reencoded.getProperty(props.getName(k)));
        const auto img = render(p);
        assert(fingerprint(img) == fingerprint(render(p))); // no stateful/random rendering
        assert(images.insert(fingerprint(img)).second); // no renamed duplicate recipes
        const int x=(i%8)*200, y=(i/8)*145;
        galleryG.setColour(juce::Colours::white); galleryG.setFont(12.0f);
        galleryG.drawText(presetName(i),x+4,y,194,22,juce::Justification::centredLeft);
        galleryG.drawImage(img,juce::Rectangle<float>((float)x+4,(float)y+24,192,112));
    }
    assert(numPresets() == 68 && families.size() == 9 && basses == 12);
    writePng(gallery,dest.getChildFile("timbre-bank.png"));

    // Old patches must not inherit new fields from updated factory defaults.
    TimbreSlotParams legacy; applyPreset(legacy,25);
    juce::DynamicObject old; old.setProperty("preset",7); old.setProperty("slope",-9.5);
    decodeParams(old,legacy);
    assert(legacy.preset==7 && legacy.slopeDbPerOct==-9.5 && legacy.spectrum==0
           && legacy.cutoffHz==0 && legacy.pitchAttackCents==0);
    old.setProperty("preset",9999); decodeParams(old,legacy); assert(legacy.preset==kPresetCustom);

    // Inharmonic strings keep the selected fundamental in tune.
    TimbreSlotParams stretched; stretched.inharmonicity=.08;
    assert(std::abs(computePartials(stretched).front().freqHz-midiNoteHz(stretched.midiNote))<1e-9);

    // Isolate filter motion from amplitude damping: late spectrum gets darker.
    TimbreSlotParams bass; applyPreset(bass,33); bass.midiNote=40;
    bass.decaySec=0; bass.hfDamp=0;
    const auto filtered=render(bass);
    const double early=centroid(filtered,3), late=centroid(filtered,128);
    printf("Analog bass centroid: %.1f Hz attack -> %.1f Hz sustain\n",early,late);
    assert(early > late * 1.2);
    bass.filterSweepOct=0;
    const auto steady=render(bass);
    assert(std::abs(centroid(steady,3)-centroid(steady,128))<1.0);

    // Table damping is audible, and switching resonator bypasses the now
    // disabled harmonic character instead of leaving hidden vowel formants.
    TimbreSlotParams bell; applyPreset(bell,9); bell.hfDamp=0;
    const auto undamped=computePartials(bell);
    bell.hfDamp=1; bell.spectrum=(int)Spectrum::VoiceAh;
    const auto damped=computePartials(bell);
    assert(damped.back().decayMul > undamped.back().decayMul);
    assert(buildVoiceModel(bell).spectrum==(int)Spectrum::Neutral);

    // A quieter raw spectrum must not boost the noise on normalisation.
    TimbreSlotParams noise; noise.numPartials=1; noise.noiseDb=-30;
    const auto plain=render(noise);
    noise.spectrum=(int)Spectrum::Piano;
    assert(fingerprint(plain)==fingerprint(render(noise)));

    // The browser never applies a sound until the leaf instrument is chosen.
    TimbreSlotParams target; applyPreset(target,7); target.midiNote=43; target.levelDb=-8;
    TimbreParamsPanel panel; panel.target=[&]() -> TimbreSlotParams& { return target; };
    int selected=0,edited=0;
    panel.onPresetChange=[&] { ++selected; };
    panel.onTimbralChange=[&] { ++edited; };
    panel.setAccent(juce::Colour(0xffb8ce80)); panel.refresh();
    auto& family=combo(panel,"timbre-family");
    auto& sub=combo(panel,"timbre-subfamily");
    auto& preset=combo(panel,"timbre-preset");
    choose(family,"Bass"); choose(sub,"Electric");
    assert(target.preset==7 && selected==0);
    preset.setSelectedId(26,juce::sendNotificationSync);
    assert(target.preset==25 && selected==1 && target.midiNote==43 && target.levelDb==-8);
    choose(combo(panel,"timbre-character"),"Picked bass");
    assert(target.preset==kPresetCustom && target.customBase==25 && edited==1);
    assert(family.getText()=="Bass" && sub.getText()=="Electric");
    assert(preset.getText().contains("Finger bass"));
    // Restore custom identity even after temporarily browsing another family.
    choose(family,"Keys"); panel.refresh();
    assert(family.getText()=="Bass" && preset.getText().contains("Finger bass"));
    for (int i=0;i<numPresets();++i)
    {
        choose(family,presetFamily(i)); choose(sub,presetSubfamily(i));
        preset.setSelectedId(i+1,juce::sendNotificationSync);
        assert(target.preset==i);
    }
    applyPreset(target,25); panel.refresh();
    panel.setSize(340,panel.preferredHeight());
    assert(panel.preferredHeight() < 380);
    for (auto* child:panel.getChildren()) assert(panel.getLocalBounds().contains(child->getBounds()));
        const int fixedHeight = panel.preferredHeight();
    for (int i=0;i<5;++i)
    {
        auto* b=dynamic_cast<juce::TextButton*>(panel.findChildWithID("timbre-section-"+juce::String(i)));
        assert(b); b->onClick();
        assert(panel.preferredHeight()==fixedHeight);
        for (auto* child:panel.getChildren())
            if (child->isVisible()) assert(panel.getLocalBounds().contains(child->getBounds()));
    }
    dynamic_cast<juce::TextButton*>(panel.findChildWithID("timbre-section-0"))->onClick();
    auto snapshot=panel.createComponentSnapshot(panel.getLocalBounds());
    juce::Image opaque(juce::Image::RGB,snapshot.getWidth(),snapshot.getHeight(),true);
    { juce::Graphics g(opaque); g.fillAll(juce::Colour(0xff222222)); g.drawImageAt(snapshot,0,0); }
    writePng(opaque,dest.getChildFile("timbre-browser.png"));
    testNoiseRendering(dest);
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    printf("PASS: %d distinct deterministic renders, %zu families, %d basses; persistence, tuning, filter and browser integration\n",
           numPresets(),families.size(),basses);
}
