#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include <array>
#include <memory>
#include <vector>

/**
 * @brief Contextual zone-3 PLAY panel for a selected VIDEO SCROLL output module.
 *
 * Shows the Video Scroll display parameters — Mode / Speed / Line Pos / Thickness
 * / Zoom / Fade / Compression + Invert + Color(RGB). There is deliberately NO
 * source selector: the source IS the module's position in the chain.
 *
 * Per-instance: setSlot(slot) rebinds every APVTS attachment to the bank
 * videoScroll{slot}_* (see vsParam()). slot < 0 unbinds (blank controls).
 * Mirrors the legacy global VideoScrollTab layout (minus SOURCE, plus toggles).
 */
class VideoScrollPage : public juce::Component
{
public:
    static constexpr int kPreferredH = 440;

    explicit VideoScrollPage(Sp3ctraAudioProcessor& proc) : processor_(proc)
    {
        // Orientation labels mirror the legacy tab (param choices are deg-based).
        modeCombo_.addItem("Scroll up",    1);
        modeCombo_.addItem("Scroll left",  2);
        modeCombo_.addItem("Scroll down",  3);
        modeCombo_.addItem("Scroll right", 4);
        addAndMakeVisible(modeCombo_);

        styleH(speedSlider_);
        styleH(linePosSlider_);
        styleH(thicknessSlider_);
        styleH(zoomSlider_, " x");
        styleH(fadeSlider_);
        styleH(compressSlider_, " fr");
        for (auto* s : sliders()) addAndMakeVisible(*s);

        // Inversion selector (matches the invertMode param order).
        invertCombo_.addItem("Off",       1);
        invertCombo_.addItem("Negative",  2);
        invertCombo_.addItem("Luminance", 3);
        addAndMakeVisible(invertCombo_);
        addAndMakeVisible(colorButton_);
    }

    /** Bind every control to the VideoScroll bank of `slot` (0..7), or unbind
     *  (slot < 0). Called by the editor when a VIDEO SCROLL block is selected. */
    void setSlot(int slot)
    {
        slot_ = slot;
        rebind();
    }

    int  slot() const noexcept { return slot_; }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        const Layout L = computeLayout(getWidth());

        auto drawSection = [&](const juce::String& txt, int y)
        {
            g.setColour(juce::Colour(0xff66cc88u));
            g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
            g.drawText(txt, kHP, y, juce::jmin(180, getWidth() - 2 * kHP), kCH,
                       juce::Justification::centredLeft, true);
        };
        auto drawLabel = [&](const juce::String& txt, int y)
        {
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
            g.setColour(juce::Colour(Sp3ctraTheme::kColText));
            g.drawText(txt, juce::Rectangle<int>(kHP, y, L.labelW, kCH),
                       juce::Justification::centredRight, true);
        };

        drawSection("SCROLL", L.secScroll);
        drawLabel("Mode",     L.yMode);
        drawLabel("Speed",    L.ySpeed);
        drawLabel("Line Pos", L.yLinePos);

        drawSection("DISPLAY", L.secDisplay);
        drawLabel("Thickness",   L.yThickness);
        drawLabel("Zoom",        L.yZoom);
        drawLabel("Fade",        L.yFade);
        drawLabel("Compression", L.yCompress);
        drawLabel("Invert",      L.yInvert);
        drawLabel("Color",       L.yColor);

        if (slot_ < 0)
        {
            g.setColour(juce::Colour(Sp3ctraTheme::kColText).withAlpha(0.5f));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
            g.drawText("Select a VIDEO SCROLL block",
                       getLocalBounds().removeFromBottom(28),
                       juce::Justification::centred, true);
        }
    }

    void resized() override
    {
        const Layout L = computeLayout(getWidth());
        const int x = L.ctrlX, w = L.ctrlW;

        const int tbW = juce::jlimit(48, Sp3ctraTheme::kTbStd, w / 3);
        for (auto* s : sliders())
            s->setTextBoxStyle(juce::Slider::TextBoxRight, false, tbW, kCH);

        modeCombo_     .setBounds(x, L.yMode,      w, kCH);
        speedSlider_   .setBounds(x, L.ySpeed,     w, kCH);
        linePosSlider_ .setBounds(x, L.yLinePos,   w, kCH);
        thicknessSlider_.setBounds(x, L.yThickness, w, kCH);
        zoomSlider_    .setBounds(x, L.yZoom,      w, kCH);
        fadeSlider_    .setBounds(x, L.yFade,      w, kCH);
        compressSlider_.setBounds(x, L.yCompress,  w, kCH);
        invertCombo_   .setBounds(x, L.yInvert,    w, kCH);
        colorButton_   .setBounds(x, L.yColor,     w, kCH);
    }

private:
    //── Geometry ──────────────────────────────────────────────────────────────
    static constexpr int kHP   = Sp3ctraTheme::kHPad;
    static constexpr int kLW    = Sp3ctraTheme::kLabelW;
    static constexpr int kGap   = Sp3ctraTheme::kGap;
    static constexpr int kCH     = Sp3ctraTheme::kControlH;
    static constexpr int kStep   = Sp3ctraTheme::kRowStep;
    static constexpr int kSecEx  = 10;

    struct Layout
    {
        int labelW, ctrlX, ctrlW;
        int secScroll, yMode, ySpeed, yLinePos;
        int secDisplay, yThickness, yZoom, yFade, yCompress, yInvert, yColor;
    };

    Layout computeLayout(int width) const
    {
        Layout L;
        L.labelW = juce::jlimit(64, kLW, width / 3);
        L.ctrlX  = kHP + L.labelW + kGap;
        L.ctrlW  = juce::jmax(70, width - kHP - L.ctrlX);

        const int top = 10;
        L.secScroll  = top;
        L.yMode      = L.secScroll + kStep;
        L.ySpeed     = L.secScroll + 2 * kStep;
        L.yLinePos   = L.secScroll + 3 * kStep;
        L.secDisplay = L.secScroll + 4 * kStep + kSecEx;
        L.yThickness = L.secDisplay + kStep;
        L.yZoom      = L.secDisplay + 2 * kStep;
        L.yFade      = L.secDisplay + 3 * kStep;
        L.yCompress  = L.secDisplay + 4 * kStep;
        L.yInvert    = L.secDisplay + 5 * kStep;
        L.yColor     = L.secDisplay + 6 * kStep;
        return L;
    }

    std::array<juce::Slider*, 6> sliders()
    {
        return { &speedSlider_, &linePosSlider_, &thicknessSlider_,
                 &zoomSlider_, &fadeSlider_, &compressSlider_ };
    }

    static void styleH(juce::Slider& s, const juce::String& suffix = "")
    {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                          Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
        if (suffix.isNotEmpty()) s.setTextValueSuffix(suffix);
    }

    //── Per-slot attachment (re)binding ───────────────────────────────────────
    void rebind()
    {
        using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
        using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
        using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;

        modeAtt_.reset();  speedAtt_.reset();  linePosAtt_.reset();
        thickAtt_.reset(); zoomAtt_.reset();   fadeAtt_.reset();
        compAtt_.reset();  invertAtt_.reset(); colorAtt_.reset();
        learnAtts_.clear();

        if (slot_ < 0) { repaint(); return; }

        auto& apvts = processor_.getAPVTS();
        modeAtt_    = std::make_unique<CA>(apvts, vsParam(slot_, "mode"),      modeCombo_);
        speedAtt_   = std::make_unique<SA>(apvts, vsParam(slot_, "speed"),     speedSlider_);
        linePosAtt_ = std::make_unique<SA>(apvts, vsParam(slot_, "linePos"),   linePosSlider_);
        thickAtt_   = std::make_unique<SA>(apvts, vsParam(slot_, "thickness"), thicknessSlider_);
        zoomAtt_    = std::make_unique<SA>(apvts, vsParam(slot_, "zoom"),      zoomSlider_);
        fadeAtt_    = std::make_unique<SA>(apvts, vsParam(slot_, "fade"),      fadeSlider_);
        compAtt_    = std::make_unique<SA>(apvts, vsParam(slot_, "compress"),   compressSlider_);
        invertAtt_  = std::make_unique<CA>(apvts, vsParam(slot_, "invertMode"), invertCombo_);
        colorAtt_   = std::make_unique<BA>(apvts, vsParam(slot_, "colorMode"),  colorButton_);

        // Right-click MIDI Learn on every play control of THIS instance.
        auto& mm = processor_.getMidiMap();
        auto learn = [&](juce::Component& c, const char* suffix)
        {
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(
                mm, c, vsParam(slot_, suffix)));
        };
        learn(modeCombo_,       "mode");
        learn(speedSlider_,     "speed");
        learn(linePosSlider_,   "linePos");
        learn(thicknessSlider_, "thickness");
        learn(zoomSlider_,      "zoom");
        learn(fadeSlider_,      "fade");
        learn(compressSlider_,  "compress");
        learn(invertCombo_,     "invertMode");
        learn(colorButton_,     "colorMode");
        repaint();
    }

    Sp3ctraAudioProcessor& processor_;
    int slot_ { -1 };

    juce::ComboBox  modeCombo_, invertCombo_;
    juce::Slider    speedSlider_, linePosSlider_, thicknessSlider_,
                    zoomSlider_, fadeSlider_, compressSlider_;
    juce::ToggleButton colorButton_;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAtt_, invertAtt_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   speedAtt_, linePosAtt_,
        thickAtt_, zoomAtt_, fadeAtt_, compAtt_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   colorAtt_;
    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoScrollPage)
};
