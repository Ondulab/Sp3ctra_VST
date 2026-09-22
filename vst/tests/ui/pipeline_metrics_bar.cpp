#include "ui/PipelineMetricsBar.h"
#include <cassert>
#include <cstdio>
static juce::Label* findLabel(juce::Component& parent,const juce::String& prefix)
{
    for(auto* child:parent.getChildren()) {
        if(auto* label=dynamic_cast<juce::Label*>(child))
            if(label->getText().startsWith(prefix))return label;
        if(auto* found=findLabel(*child,prefix))return found;
    }
    return nullptr;
}
static juce::Label* labelStarting(PipelineMetricsBar& bar,const juce::String& prefix)
{auto* label=findLabel(bar,prefix);assert(label);return label;}
int main(int argc,char**argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    PipelineMetricsBar bar;
    bar.setSize(1024,PipelineMetricsBar::kHeight);
    bar.sample(1000,255,44100,32,0,0,0,0);
    for(int i=0;i<525;++i)pipeline_metric_hit(PIPE_RX);
    for(int c=0;c<8;++c)for(int i=0;i<500+c;++i)pipeline_metric_hit(PIPE_CHAIN+c);
    for(int e=0;e<4;++e)for(int i=0;i<500;++i)pipeline_metric_hit(PIPE_FEED+e);
    pipeline_metric_hit(PIPE_SEND+2*8+3);
    bar.sample(1500,255,44100,32,689,8,28,0);
    assert(labelStarting(bar,"INPUT")->getText().contains("1050 lines/s"));
    assert(labelStarting(bar,"AUDIO")->getText().contains("1378 blocks/s"));
    assert(labelStarting(bar,"LINK")->getText().contains("16 changes/s"));
    assert(labelStarting(bar,"VIDEO")->getText().contains("56.0 fps"));
    assert(labelStarting(bar,"C4")->getText().contains("1006 l/s"));
    assert(labelStarting(bar,"C4")->getTooltip().contains("WAVE: 2"));
    auto* content=labelStarting(bar,"INPUT")->getParentComponent();
    assert(content->getWidth()>bar.getWidth()); // narrow windows scroll, no truncation
    for(auto* child:content->getChildren()) {
        assert(content->getLocalBounds().contains(child->getBounds()));
        assert(child->getY()==0); // exactly one row
        if(auto* label=dynamic_cast<juce::Label*>(child)) {
            juce::GlyphArrangement glyphs;
            glyphs.addLineOfText(label->getFont(),label->getText(),0,0);
            assert(glyphs.getBoundingBox(0,-1,true).getWidth()<=label->getWidth());
        }
    }
    assert(labelStarting(bar,"C1")->findColour(juce::Label::textColourId)==ChainIdentity::colour(0));
    assert(labelStarting(bar,"VIDEO")->findColour(juce::Label::textColourId)==moduleColour(ModuleType::VideoScroll));
    assert(labelStarting(bar,"STRAL")->findColour(juce::Label::textColourId)==moduleColour(ModuleType::LuxStral));
    bar.setSize(2560,PipelineMetricsBar::kHeight);
    if(argc>1){auto frame=bar.createComponentSnapshot(bar.getLocalBounds());juce::File file(argv[1]);auto out=file.createOutputStream();assert(out);out->setPosition(0);out->truncate();juce::PNGImageFormat png;assert(png.writeImageToStream(frame,*out));}
    // A slow GUI reports the window average, then returns to zero on idle.
    bar.sample(3000,15,44100,32,689,8,28,0);
    assert(labelStarting(bar,"INPUT")->getText().contains("0 lines/s"));
    assert(!labelStarting(bar,"C8")->isVisible());
    assert(labelStarting(bar,"LINK")->getText().contains("0 changes/s"));
    puts("PASS: footer actual rates, zero on idle, chain/send detail and single coloured row, scrollable at 1024 px");
}
