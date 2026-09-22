#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../UITheme.h"
#include "ChainIdentity.h"
#include "ModuleCatalog.h"
#include "../utils/pipeline_metrics.h"
#include "../utils/CounterRate.h"
#include <array>

// Display-only, sampled by the existing editor timer at 2 Hz. All formatting
// stays on the message thread. Core counters are shared; callback/LINK/video
// counts belong to the displayed processor/editor.
class PipelineMetricsBar : public juce::Component
{
public:
    static constexpr int kHeight = 28;
    PipelineMetricsBar()
    {
        setOpaque(true);
        addAndMakeVisible(viewport_);
        viewport_.setViewedComponent(&content_,false);
        viewport_.setScrollBarsShown(false,true);
        viewport_.setScrollBarThickness(4);
        for (auto* group : {&top_, &feeds_})
            for (auto& label : *group) setup(label);
        for (auto& label : chains_) setup(label);
        setup(holdsLabel_);
        top_[0].setColour(juce::Label::textColourId,moduleColour(ModuleType::Sp3ctra));
        top_[1].setColour(juce::Label::textColourId,juce::Colour(0xff5a9de0)); // AUDIO MIX header
        top_[2].setColour(juce::Label::textColourId,juce::Colour(Sp3ctraTheme::kColMod));
        top_[3].setColour(juce::Label::textColourId,moduleColour(ModuleType::VideoScroll));
        const ModuleType engines[]={ModuleType::LuxStral,ModuleType::LuxSynth,ModuleType::LuxWave,ModuleType::LuxGrain};
        for(int e=0;e<4;++e)feeds_[e].setColour(juce::Label::textColourId,moduleColour(engines[e]));
        for(int c=0;c<8;++c)chains_[c].setColour(juce::Label::textColourId,ChainIdentity::colour(c));
        holdsLabel_.setTooltip("Mix reads held because a producer was updating a staging buffer. The previous feed is retained. This is not an audio underrun counter.");
        top_[0].setTooltip("Complete sensor lines received per second. Internal media/player sources are counted in their chains below.");
        top_[1].setTooltip("Measured callback blocks per second; ms is the configured block duration, not end-to-end latency.");
        top_[2].setTooltip("Actual VIDEO -> AUDIO weight changes per second, independent of video refresh. A square 8 Hz LFO normally produces 16 changes/s. Continuous modulation has a different count.");
        top_[3].setTooltip("Completed VIDEO MIX images per second, not physical screen refresh. Static images need no new publication.");
        for (int e=0;e<4;++e)
            feeds_[e].setTooltip("Non-empty mix publications to this synthesis engine per second. LuxStral/LuxWave can republish unchanged input; LuxSynth/LuxGrain skip unchanged generations. This is not the oscillator frequency or the audio sample rate.");
    }

    /** `engineMask` bit e (STRAL/SYNTH/WAVE/GRAIN order) = that engine has at
     *  least one send placed in a chain; an engine without a send is hidden
     *  (same rule as the AUDIO MIX strips: no send = no feed, nothing to read). */
    void sample(double nowMs, unsigned chainMask, unsigned engineMask, double sampleRate,
                int blockSize, uint64_t audioBlocks, uint64_t linkChanges,
                uint64_t videoFrames, uint64_t holds)
    {
        if (lastMs_ > 0 && nowMs - lastMs_ < 500.0) return;
        const double seconds = lastMs_ > 0 ? (nowMs-lastMs_)*0.001 : 0.0;
        lastMs_ = nowMs;
        uint64_t counts[PIPE_METRIC_COUNT]; pipeline_metrics_read(counts);
        std::array<double, PIPE_METRIC_COUNT> rates;
        for (size_t i=0;i<rates.size();++i) rates[i]=core_[i].sample(counts[i],seconds);
        set(top_[0], "INPUT  " + number(rates[PIPE_RX]) + " lines/s");
        const auto audio = audio_.sample(audioBlocks,seconds);
        set(top_[1], "AUDIO  " + number(audio) + " blocks/s  |  "
            + (sampleRate > 0 && blockSize > 0 ? juce::String(1000.0*blockSize/sampleRate,2) + " ms" : "--"));
        set(top_[2], "LINK  " + number(link_.sample(linkChanges,seconds)) + " changes/s");
        set(top_[3], "VIDEO  " + juce::String(video_.sample(videoFrames,seconds),1) + " fps");
        const double held=holds_.sample(holds,seconds);
        const char* names[]={"STRAL","SYNTH","WAVE","GRAIN"};
        for (int c=0;c<8;++c)
        {
            const bool exists=(chainMask & (1u<<c)) != 0;
            chains_[c].setVisible(exists);
            const double rate=rates[PIPE_CHAIN+c];
            set(chains_[c], "C" + juce::String(c+1) + "  " + (exists ? number(rate)+" l/s" : "--"));
            juce::String tip="Chain " + juce::String(c+1) + ": lines reaching the end of the chain. Ownership spans are not added together. Includes processed repeats and effect tails.\n";
            tip += rate > 0 ? "Mean interval: " + juce::String(1000.0/rate,2) + " ms (not end-to-end latency).\n" : "No completed line in this measurement window.\n";
            tip += "Conditioned lines published to each send (lines/s):";
            for(int e=0;e<4;++e)
                if (engineMask & (1u<<e))
                    tip += "\n" + juce::String(names[e]) + ": " + number(rates[PIPE_SEND+e*8+c]);
            chains_[c].setTooltip(tip);
        }
        for(int e=0;e<4;++e)
        {
            feeds_[e].setVisible((engineMask & (1u<<e)) != 0);
            set(feeds_[e],juce::String(names[e])+"  "+number(rates[PIPE_FEED+e])+" feeds/s");
        }
        // Holds are retries of a concurrently-written staging buffer, not xruns.
        set(holdsLabel_,"HOLDS  "+number(held)+"/s");
        holdsLabel_.setColour(juce::Label::textColourId,juce::Colour(held > 0 ? 0xffffb020 : 0xff9aa6ba));
        resized();
    }
    void resized() override
    {
        viewport_.setBounds(getLocalBounds().withTrimmedTop(1));
        int x=4;
        auto place=[&](juce::Label& label) {
            if (! label.isVisible()) return;
            const int width=(int)std::ceil(juce::GlyphArrangement::getStringWidth(label.getFont(),label.getText()))+16;
            label.setBounds(x,0,width,23);
            x += width;
        };
        for(auto& label:top_)place(label);
        for(auto& label:chains_)place(label);
        for(auto& label:feeds_)place(label);
        place(holdsLabel_);
        content_.setSize(juce::jmax(getWidth(),x+4),23);
    }
    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff17171e));
        g.setColour(juce::Colour(0xff353541));
        g.fillRect(0,0,getWidth(),1);
    }
private:
    void setup(juce::Label& label)
    {
        label.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSmall)));
        label.setColour(juce::Label::textColourId,juce::Colour(0xffc2c7d0));
        label.setBorderSize(juce::BorderSize<int>(0));
        label.setText("--",juce::dontSendNotification);
        content_.addAndMakeVisible(label);
    }
    static juce::String number(double value) { return juce::String(juce::roundToInt(value)); }
    static void set(juce::Label& label,const juce::String& text)
    { if(label.getText()!=text)label.setText(text,juce::dontSendNotification); }
    juce::Component content_;
    juce::Viewport viewport_;
    std::array<juce::Label,4> top_,feeds_;
    std::array<juce::Label,8> chains_;
    juce::Label holdsLabel_;
    std::array<CounterRate,PIPE_METRIC_COUNT> core_;
    CounterRate audio_,link_,video_,holds_;
    double lastMs_=0;
};
