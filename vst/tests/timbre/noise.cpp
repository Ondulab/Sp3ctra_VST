// Render through NotePainter and measure with the production inverse-dB
// decoder. These are oscillator-bank power tests, not peak-pixel comparisons.
#include "image/TimbreGenRenderer.h"
#include "processing/image_pipeline_stages.h"
#include "luxsampler/ScorePlayerService.h"
#include "config/config_loader.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace timbregen;
namespace {
struct Strip { juce::Image image; double reference, pxPerSec; };
Strip strip(const TimbreSlotParams& p, double widthMM=.3, int height=1728,
            int columns=240, double pxPerSec=1000, double frequency=220, int clipX0=0)
{
    juce::Image image(juce::Image::RGB,columns,height,true);
    { juce::Graphics g(image); g.fillAll(juce::Colours::white); }
    BandGeom band;
    band.xMin=clipX0; band.xMax=columns; band.yBottom=height; band.heightPx=height; band.yBot=height;
    band.pxPerSec=pxPerSec; band.t1=columns/pxPerSec;
    InkSettings ink; ink.minFreq=65; ink.maxFreq=16640; ink.lineWidthMM=widthMM;
    ink.dpiY=height*25.4/SCORE_CIS_HEIGHT_MM;
    const auto vm=buildVoiceModel(p); ink.maxDb=vm.maxAmpDb;
    NoteInk note; note.f0Hz=frequency; note.endSec=band.t1; note.noteIndex=17;
    NotePainter painter(image,band,ink); painter.paint(vm,note);
    return {image,height/(SCORE_CIS_HEIGHT_MM*400.0/25.4),pxPerSec};
}
std::vector<float> decode(const Strip& s,int x)
{
    std::vector<float> bins((size_t)s.image.getHeight());
    for(int y=0;y<s.image.getHeight();++y) bins[(size_t)y]=1.0f-s.image.getPixelAt(x,y).getRed()/255.0f;
    img_stage_apply_db_decode(bins.data(),(int)bins.size(),50);
    return bins;
}
double power(const Strip& s,int x,double minimumHz=0)
{
    const auto bins=decode(s,x); double p=0;
    for(size_t y=0;y<bins.size();++y)
    {
        const double f=65.0*std::pow(256.0,1.0-(double)y/bins.size());
        if(f>=minimumHz) p+=bins[y]*bins[y];
    }
    return p/s.reference;
}
double averagePower(const Strip& s,int begin,int end)
{
    double p=0; for(int x=begin;x<end;++x) p+=power(s,x); return p/(end-begin);
}
void save(const Strip& s,const juce::File& f)
{
    auto out=f.createOutputStream(); assert(out);out->setPosition(0);out->truncate();
    assert(juce::PNGImageFormat().writeImageToStream(s.image,*out));
}
}
void testNoiseRendering(const juce::File& dest)
{
    const bool verify=std::getenv("TIMBRE_MEASURE_ONLY")==nullptr;
    TimbreSlotParams sine; applyPreset(sine,0);
    double low=1e9,high=0;
    for(const int height:{864,3456}) for(const double width:{.10,.30,.80})
    {
        const auto s=strip(sine,width,height);
        const double p=averagePower(s,60,150);
        low=std::min(low,p);high=std::max(high,p);
        printf("Partial power: height=%d width=%.2f -> %.5f\n",height,width,p);
    }
    printf("Width / DPI level variation: %.3f dB\n",10*std::log10(high/low));
    if(verify) { assert(low>.94 && high<1.06); assert(10*std::log10(high/low)<.3); }

    // Sweep a narrow partial across a raster cell without introducing vibrato AM.
    low=1e9;high=0;
    for(int phase=0;phase<12;++phase)
    {
        const double hz=65*std::pow(256.0,1.0-(500.0+phase/12.0)/864.0);
        const double moved=power(strip(sine,.1,864,240,1000,hz),100);
        low=std::min(low,moved);high=std::max(high,moved);
    }
    printf("Subpixel level variation: %.3f dB\n",10*std::log10(high/low));
    if(verify) assert(10*std::log10(high/low)<.3);

    TimbreSlotParams noise; noise.numPartials=0; noise.noiseDb=-6; noise.noiseTilt=0;noise.attackMs=8;
    const auto white=strip(noise);
    const double rmsDb=10*std::log10(averagePower(white,20,180));
    printf("White noise: requested -6 dB total, decoded %.3f dB\n",rmsDb);
    if(verify) assert(std::abs(rmsDb+6)<.6);
    const auto clipped=strip(noise,.3,1728,240,1000,220,101);
    for(int x=101;x<240;++x) for(int y=0;y<1728;++y)
        if(verify) assert(white.image.getPixelAt(x,y)==clipped.image.getPixelAt(x,y));
    const auto fine=strip(noise,.3,3456);
    const double fineDb=10*std::log10(averagePower(fine,20,180));
    if(verify) assert(std::abs(fineDb-rmsDb)<.3);

    // Use the actual SCORE image -> CIS-frame reader too, including its
    // row resampling, before the production dB decoder. No audio device opens.
    const float oldLo=g_sp3ctra_config.low_frequency,oldHi=g_sp3ctra_config.high_frequency;
    g_sp3ctra_config.low_frequency=65;g_sp3ctra_config.high_frequency=16640;
    const auto frames=ScorePlayerService::buildFramesFromImage(white.image,{},65,16640,false);
    assert(frames.size()==240);
    double playedPower=0;
    std::vector<float> amplitudes(LuxSamplerConstants::MAX_PIXELS);
    for(int x=20;x<180;++x)
    {
        for(size_t i=0;i<amplitudes.size();++i) amplitudes[i]=1.0f-frames[(size_t)x].R[i]/255.0f;
        img_stage_apply_db_decode(amplitudes.data(),(int)amplitudes.size(),50);
        for(float a:amplitudes) playedPower+=a*a;
    }
    const double playedDb=10*std::log10(playedPower/160);
    printf("SCORE playback-frame noise power: %.3f dB\n",playedDb);
    if(verify) assert(std::abs(playedDb+6)<.6);
    g_sp3ctra_config.low_frequency=oldLo;g_sp3ctra_config.high_frequency=oldHi;

    // At the old 4 ms boundaries adjacent samples must no longer jump.
    const auto smooth=strip(noise,.3,864,1300,20000);
    double maxJump=0;
    for(int cell=3;cell<=12;++cell)
    {
        const int boundary=cell*80;
        const auto a=decode(smooth,boundary-1),b=decode(smooth,boundary);
        double diff=0,p=0;
        for(size_t i=0;i<a.size();++i) { diff+=(b[i]-a[i])*(b[i]-a[i]);p+=a[i]*a[i]; }
        maxJump=std::max(maxJump,std::sqrt(diff/p));
    }
    printf("Largest 4 ms boundary amplitude jump: %.4f\n",maxJump);
    if(verify) assert(maxJump<.025);

    // Noise must obey both the vowel shape and the patch low-pass.
    noise.spectrum=(int)Spectrum::VoiceAh; noise.cutoffHz=1600;
    const auto vocal=strip(noise);
    const double hf=power(vocal,100,6000)/power(vocal,100);
    printf("Filtered vocal noise power above 6 kHz: %.8f\n",hf);
    if(verify) assert(hf<.001);

    TimbreSlotParams burst; burst.numPartials=0;burst.burstDb=-3;burst.burstMs=12;
    const auto hit=strip(burst,.3,864,800,20000);
    double peak=0;for(int x=0;x<400;++x) peak=std::max(peak,power(hit,x));
    const double first=power(hit,0)/peak;
    printf("Burst first-column / peak power: %.8f\n",first);
    if(verify) assert(first<.0001);

    TimbreSlotParams marimba;applyPreset(marimba,19);
    TimbreSlotParams voice;applyPreset(voice,16);
    if(verify) { assert(!buildVoiceModel(marimba).hasBurst());assert(!buildVoiceModel(voice).hasNoise()); }
    save(strip(marimba,.3,864,500,10000,261.625565),dest.getChildFile("marimba-attack.png"));
    save(strip(voice),dest.getChildFile("vocal-ah.png"));
    save(white,dest.getChildFile("white-noise.png"));
    puts(verify ? "PASS: integrated noise level, smooth grains/onsets, harmonic width and subpixel power"
                : "MEASUREMENTS ONLY: baseline assertions disabled");
}
