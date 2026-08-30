#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>            // juce::ColourSelector (Background picker)
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../ui/ModuleCatalog.h"                      // moduleColour()
#include "../ui/ModuleEditorChrome.h"
#include "../ui/Sp3ctraBarSlider.h"
#include "../midi/MidiLearnAttachment.h"
#include "VideoScrollViewportEditor.h"
#include "VideoScrollPreviewSource.h"
#include <array>
#include <functional>
#include <memory>
#include <vector>

/**
 * @brief Contextual zone-3 page for ONE VIDEO SCROLL output instance.
 *
 * Standard module-page skeleton (ModuleEditorChrome, 2026-08-28):
 *
 *   ┌ VIEWPORT ──────────────────────────────── 244° · 0.98× · x 0 y 0 ┐
 *   │  the output window as a live thumbnail + the grabbable vignette   │
 *   │  (drag = Center X/Y, corner = Zoom, lever = Rotation, line = Pos) │
 *   └───────────────────────────────────────────────────────────────────┘
 *    Rotation   Zoom   Center X   Center Y   Line Pos          (geometry)
 *    Speed  Thickness  Compression  Fade  Blur  Gamma          (look)
 *    --- VIDEO SCROLL ---
 *    Invert   Color   Background                               (display law)
 *
 * The pad (VideoScrollViewportEditor) and the boxes are bound to the same
 * parameters, so either can drive the other. There is deliberately NO source
 * selector: the source IS the module's position in the chain. The frame
 * colour is the Background swatch (a CallOutBox ColourSelector, D4).
 *
 * Per-instance: setSlot(slot) rebinds every APVTS attachment to the bank
 * videoScroll{slot}_* (see vsParam()). slot < 0 unbinds (blank controls).
 * Hosted twice: as THE page of the selected chain tab, and once per output
 * inside VideoScrollAllPage (the ALL tab) — keep it timer-free: the editor's
 * timer drives previewTick() (live thumbnail refresh).
 */
class VideoScrollPage : public juce::Component,
                        private juce::AudioProcessorValueTreeState::Listener,
                        private juce::AsyncUpdater
{
public:
    static inline const uint32_t kAccentARGB = moduleColour(ModuleType::VideoScroll).getARGB();

    /// Frame + geometry row + look row (the "editors" block above the caption).
    static constexpr int kEditorsH = VideoScrollViewportEditor::kPreferredH
                                   + ModuleChrome::kBelowFrameH
                                   + ModuleChrome::kRowGap + ModuleChrome::kBoxRowH;
    /// + "--- VIDEO SCROLL ---" + the Invert / Color / Background row.
    static constexpr int kPreferredH = ModuleChrome::pageHeight(kEditorsH, 1);

    explicit VideoScrollPage(Sp3ctraAudioProcessor& proc)
        : processor_(proc),
          viewport_(proc.getAPVTS(), juce::Colour(kAccentARGB))
    {
        viewport_.setMidiMap(&proc.getMidiMap());
        addAndMakeVisible(viewport_);
        // The box label of the setting being edited takes the control colour
        // (the pad lights its element and card at the same time).
        viewport_.onActiveParamChanged = [this] { repaintLabels(); };

        // Continuous orientation (degrees, clockwise): 0 = scroll up, 90 = new
        // lines at the left, 180 = scroll down, 270 = at the right.
        rotationSlider_.setTextValueSuffix(juce::String::fromUTF8(" \xC2\xB0"));
        zoomSlider_    .setTextValueSuffix(" x");
        compressSlider_.setTextValueSuffix(" fr");
        for (auto* s : sliders()) addAndMakeVisible(*s);

        // Inversion selector (matches the invertMode param order).
        invertCombo_.addItem("Off",       1);
        invertCombo_.addItem("Negative",  2);
        invertCombo_.addItem("Luminance", 3);
        addAndMakeVisible(invertCombo_);
        addAndMakeVisible(colorButton_);

        // Background = frame colour, painted wherever the band doesn't reach.
        // Picked in the SOURCE space (white = the chain's paper) and pushed
        // through Invert / Color like the image (white + Luminance = black frame).
        bgSwatch_.setTooltip(juce::String::fromUTF8(
            "Frame colour (outside the band)\n"
            "Follows Invert / Color like the image \xE2\x80\x94 white + Luminance = black frame"));
        bgSwatch_.onClick = [this] { openBgPicker(); };
        addAndMakeVisible(bgSwatch_);
    }

    ~VideoScrollPage() override
    {
        cancelPendingUpdate();
        unlistenBg();
    }

    /** Bind every control to the VideoScroll bank of `slot` (0..7), or unbind
     *  (slot < 0). Called by the editor when a VIDEO SCROLL block is selected. */
    void setSlot(int slot)
    {
        unlistenBg();
        slot_ = slot;
        rebind();
        listenBg();
        refreshSwatch();
    }

    int  slot() const noexcept { return slot_; }

    /** The VIEWPORT pad's live thumbnail / view aspect (the zone-4 mixer).
     *  Null = schematic pad. */
    void setPreviewSource(VideoScrollPreviewSource* src) { viewport_.setPreviewSource(src); }

    /** Editor timer (20 Hz): refresh the thumbnail when a frame was published. */
    void previewTick() { viewport_.previewTick(); }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        const juce::Colour accent(kAccentARGB);
        ModuleChrome::drawSectionCaption(g, ModuleChrome::kPageTop + kEditorsH, getWidth(),
                                         accent, "VIDEO SCROLL");

        // Labels: module colour, except the setting being edited (from the
        // pad, its box or MIDI) which takes the control colour — the same
        // "lit" grammar as the pad's element and card.
        using P = VideoScrollViewportEditor::Param;
        const P active = viewport_.activeParam();
        auto label = [&](const juce::Component& box, P p, const char* text)
        {
            const auto bb = box.getBounds();
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
            g.setColour(active == p ? Sp3ctraHandles::colour() : accent.withAlpha(0.6f));
            g.drawText(text, bb.getX(), bb.getY() - ModuleChrome::kLabelH, bb.getWidth(),
                       ModuleChrome::kLabelH, juce::Justification::centred, false);
        };
        label(rotationSlider_,  P::Rotation,  "Rotation");
        label(zoomSlider_,      P::Zoom,      "Zoom");
        label(centerXSlider_,   P::CenterX,   "Center X");
        label(centerYSlider_,   P::CenterY,   "Center Y");
        label(linePosSlider_,   P::LinePos,   "Line Pos");

        label(speedSlider_,     P::Speed,     "Speed");
        label(thicknessSlider_, P::Thickness, "Thickness");
        label(compressSlider_,  P::Compress,  "Compression");
        label(fadeSlider_,      P::Fade,      "Fade");
        label(blurSlider_,      P::Blur,      "Blur");
        label(gammaSlider_,     P::Gamma,     "Gamma");

        label(invertCombo_,     P::Paper,     "Invert");
        label(colorButton_,     P::Paper,     "Color");
        label(bgSwatch_,        P::Paper,     "Background");

        if (slot_ < 0)
        {
            g.setColour(juce::Colour(Sp3ctraTheme::kColText).withAlpha(0.5f));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
            g.drawText("Select a VIDEO SCROLL block",
                       getLocalBounds().removeFromBottom(ModuleChrome::kBoxRowH),
                       juce::Justification::centred, true);
        }
    }

    void resized() override
    {
        const int pad = ModuleChrome::kPagePad;
        const int w   = getWidth() - 2 * pad;

        int y = ModuleChrome::kPageTop;
        viewport_.setBounds(pad, y, w, VideoScrollViewportEditor::kPreferredH);
        y += VideoScrollViewportEditor::kPreferredH + ModuleChrome::kRowGap;

        // Geometry row — the same parameters the pad's handles drive.
        ModuleChrome::layoutBoxRow(juce::Rectangle<int>(pad, y, w, ModuleChrome::kBoxRowH),
                                   { &rotationSlider_, &zoomSlider_, &centerXSlider_,
                                     &centerYSlider_, &linePosSlider_ });
        y += ModuleChrome::kBoxRowH + ModuleChrome::kRowGap;

        // Look row.
        ModuleChrome::layoutBoxRow(juce::Rectangle<int>(pad, y, w, ModuleChrome::kBoxRowH),
                                   { &speedSlider_, &thicknessSlider_, &compressSlider_,
                                     &fadeSlider_, &blurSlider_, &gammaSlider_ });
        y += ModuleChrome::kBoxRowH;          // = kPageTop + kEditorsH

        // Display-law row under the section caption — label-above idiom; the
        // controls keep their natural widths instead of stretching to the page.
        y += ModuleChrome::kSectionCaptionH + ModuleChrome::kLabelH;
        int x = pad;
        invertCombo_.setBounds(x, y, juce::jmin(kComboW, w), ModuleChrome::kBoxH);
        x += invertCombo_.getWidth() + ModuleChrome::kBoxGap;
        colorButton_.setBounds(x, y, kToggleW, ModuleChrome::kBoxH);
        x += kToggleW + ModuleChrome::kBoxGap;
        bgSwatch_.setBounds(x, y, juce::jmin(kSwatchW, juce::jmax(40, getWidth() - pad - x)),
                            ModuleChrome::kBoxH);
    }

private:
    //── Geometry ──────────────────────────────────────────────────────────────
    static constexpr int kComboW  = 160;   // a combo never stretches to the page width
    static constexpr int kToggleW = 48;    // sliding switch
    static constexpr int kSwatchW = 120;   // compact swatch — the selector lives in a CallOutBox

    std::array<Sp3ctraBarSlider*, 11> sliders()
    {
        return { &rotationSlider_, &speedSlider_, &linePosSlider_, &thicknessSlider_,
                 &zoomSlider_, &centerXSlider_, &centerYSlider_,
                 &fadeSlider_, &blurSlider_, &gammaSlider_, &compressSlider_ };
    }

    //── Background swatch + picker ────────────────────────────────────────────
    /** Flat colour tile with its hex readout; click → CallOutBox picker. */
    class BgSwatchButton : public juce::Button
    {
    public:
        BgSwatchButton() : juce::Button("background") {}

        void setSwatchColour(juce::Colour c)
        {
            if (c != colour_) { colour_ = c; repaint(); }
        }

        void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override
        {
            const auto r = getLocalBounds().toFloat().reduced(0.5f);
            g.setColour(colour_);
            g.fillRoundedRectangle(r, 3.f);
            g.setColour(juce::Colour(Sp3ctraTheme::kColBorder)
                            .brighter((isMouseOver || isButtonDown) ? 0.8f : 0.f));
            g.drawRoundedRectangle(r, 3.f, 1.f);
            g.setColour(colour_.getPerceivedBrightness() > 0.55f ? juce::Colours::black
                                                                  : juce::Colours::white);
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
            g.drawText("#" + colour_.toDisplayString(false).toUpperCase(),
                       getLocalBounds(), juce::Justification::centred, false);
        }

    private:
        juce::Colour colour_ { juce::Colours::white };
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BgSwatchButton)
    };

    /** CallOutBox content: the full ColourSelector, reporting every change. */
    class BgPicker : public juce::Component,
                     private juce::ChangeListener
    {
    public:
        BgPicker(juce::Colour initial, std::function<void(juce::Colour)> onChange)
            : onChange_(std::move(onChange))
        {
            selector_.setName("Background");
            selector_.setCurrentColour(initial, juce::dontSendNotification);
            selector_.addChangeListener(this);
            addAndMakeVisible(selector_);
            setSize(260, 300);
        }
        ~BgPicker() override { selector_.removeChangeListener(this); }
        void resized() override { selector_.setBounds(getLocalBounds().reduced(6)); }

    private:
        void changeListenerCallback(juce::ChangeBroadcaster*) override
        {
            if (onChange_) onChange_(selector_.getCurrentColour());
        }
        juce::ColourSelector selector_ {
            juce::ColourSelector::showColourAtTop
          | juce::ColourSelector::showSliders
          | juce::ColourSelector::showColourspace };
        std::function<void(juce::Colour)> onChange_;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BgPicker)
    };

    float readParam(const char* suffix, float def) const
    {
        if (slot_ < 0) return def;
        if (auto* v = processor_.getAPVTS().getRawParameterValue(vsParam(slot_, suffix)))
            return v->load();
        return def;
    }

    juce::Colour currentBgColour() const
    {
        return juce::Colour::fromFloatRGBA(readParam("bgR", 1.f), readParam("bgG", 1.f),
                                           readParam("bgB", 1.f), 1.f);
    }

    void writeBg(juce::Colour c)
    {
        if (slot_ < 0) return;
        auto& apvts = processor_.getAPVTS();
        const std::pair<const char*, float> parts[] = {
            { "bgR", c.getFloatRed() }, { "bgG", c.getFloatGreen() }, { "bgB", c.getFloatBlue() } };
        for (const auto& [suffix, v] : parts)
            if (auto* p = apvts.getParameter(vsParam(slot_, suffix)))
                p->setValueNotifyingHost(p->convertTo0to1(juce::jlimit(0.f, 1.f, v)));
        bgSwatch_.setSwatchColour(c);
    }

    void openBgPicker()
    {
        if (slot_ < 0) return;
        // The page may die while the callout is up (ALL view rebuilt by a rack
        // edit) — never write through a dangling this.
        juce::Component::SafePointer<VideoScrollPage> safe(this);
        auto content = std::make_unique<BgPicker>(currentBgColour(), [safe](juce::Colour c)
        {
            if (safe != nullptr) safe->writeBg(c);
        });
        juce::CallOutBox::launchAsynchronously(std::move(content),
                                               bgSwatch_.getScreenBounds(), nullptr);
    }

    void refreshSwatch()
    {
        bgSwatch_.setSwatchColour(currentBgColour());
        viewport_.repaint();   // the pad's paper follows the frame colour / law
    }

    // APVTS listener (any thread — automation / MIDI) → repaint the swatch and
    // the pad's paper on the message thread.
    void parameterChanged(const juce::String&, float) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { refreshSwatch(); }

    static constexpr const char* kPaperParams[] = { "bgR", "bgG", "bgB", "invertMode", "colorMode" };

    void listenBg()
    {
        if (slot_ < 0) return;
        auto& apvts = processor_.getAPVTS();
        for (const char* s : kPaperParams)
            apvts.addParameterListener(vsParam(slot_, s), this);
        listeningSlot_ = slot_;
    }

    void unlistenBg()
    {
        if (listeningSlot_ < 0) return;
        auto& apvts = processor_.getAPVTS();
        for (const char* s : kPaperParams)
            apvts.removeParameterListener(vsParam(listeningSlot_, s), this);
        listeningSlot_ = -1;
    }

    //── Per-slot attachment (re)binding ───────────────────────────────────────
    void rebind()
    {
        using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
        using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
        using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;

        rotationAtt_.reset(); speedAtt_.reset();  linePosAtt_.reset();
        thickAtt_.reset();   zoomAtt_.reset();    centerXAtt_.reset(); centerYAtt_.reset();
        fadeAtt_.reset();    blurAtt_.reset();    gammaAtt_.reset();
        compAtt_.reset();    invertAtt_.reset();  colorAtt_.reset();
        learnAtts_.clear();
        viewport_.setSlot(slot_);

        if (slot_ < 0) { repaint(); return; }

        auto& apvts = processor_.getAPVTS();
        rotationAtt_ = std::make_unique<SA>(apvts, vsParam(slot_, "rotation"), rotationSlider_);
        speedAtt_   = std::make_unique<SA>(apvts, vsParam(slot_, "speed"),     speedSlider_);
        linePosAtt_ = std::make_unique<SA>(apvts, vsParam(slot_, "linePos"),   linePosSlider_);
        thickAtt_   = std::make_unique<SA>(apvts, vsParam(slot_, "thickness"), thicknessSlider_);
        zoomAtt_    = std::make_unique<SA>(apvts, vsParam(slot_, "zoom"),      zoomSlider_);
        centerXAtt_ = std::make_unique<SA>(apvts, vsParam(slot_, "centerX"),   centerXSlider_);
        centerYAtt_ = std::make_unique<SA>(apvts, vsParam(slot_, "centerY"),   centerYSlider_);
        fadeAtt_    = std::make_unique<SA>(apvts, vsParam(slot_, "fade"),      fadeSlider_);
        blurAtt_    = std::make_unique<SA>(apvts, vsParam(slot_, "blur"),      blurSlider_);
        gammaAtt_   = std::make_unique<SA>(apvts, vsParam(slot_, "gamma"),     gammaSlider_);
        compAtt_    = std::make_unique<SA>(apvts, vsParam(slot_, "compress"),   compressSlider_);
        invertAtt_  = std::make_unique<CA>(apvts, vsParam(slot_, "invertMode"), invertCombo_);
        colorAtt_   = std::make_unique<BA>(apvts, vsParam(slot_, "colorMode"),  colorButton_);

        // Right-click MIDI Learn on every play control of THIS instance (the
        // pad resolves its own parameter from the handle under the pointer).
        auto& mm = processor_.getMidiMap();
        auto learn = [&](juce::Component& c, const char* suffix)
        {
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(
                mm, c, vsParam(slot_, suffix)));
        };
        learn(rotationSlider_,  "rotation");
        learn(speedSlider_,     "speed");
        learn(linePosSlider_,   "linePos");
        learn(thicknessSlider_, "thickness");
        learn(zoomSlider_,      "zoom");
        learn(centerXSlider_,   "centerX");
        learn(centerYSlider_,   "centerY");
        learn(fadeSlider_,      "fade");
        learn(blurSlider_,      "blur");
        learn(gammaSlider_,     "gamma");
        learn(compressSlider_,  "compress");
        learn(invertCombo_,     "invertMode");
        learn(colorButton_,     "colorMode");
        repaint();
    }

    /** Repaint the label strips only (not the pad, not the boxes). */
    void repaintLabels()
    {
        const juce::Component* const boxes[] = { &rotationSlider_, &zoomSlider_, &centerXSlider_,
                                                 &centerYSlider_, &linePosSlider_, &speedSlider_,
                                                 &thicknessSlider_, &compressSlider_, &fadeSlider_,
                                                 &blurSlider_, &gammaSlider_, &invertCombo_,
                                                 &colorButton_, &bgSwatch_ };
        for (const juce::Component* c : boxes)
        {
            const auto bb = c->getBounds();
            repaint(bb.getX(), bb.getY() - ModuleChrome::kLabelH, bb.getWidth(), ModuleChrome::kLabelH);
        }
    }

    Sp3ctraAudioProcessor& processor_;
    int slot_          { -1 };
    int listeningSlot_ { -1 };   // slot whose paper params we listen to (−1 = none)

    VideoScrollViewportEditor viewport_;

    juce::ComboBox  invertCombo_;
    Sp3ctraBarSlider rotationSlider_, speedSlider_, linePosSlider_, thicknessSlider_,
                     zoomSlider_, centerXSlider_, centerYSlider_,
                     fadeSlider_, blurSlider_, gammaSlider_, compressSlider_;
    juce::ToggleButton colorButton_;
    BgSwatchButton     bgSwatch_;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> invertAtt_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   rotationAtt_, speedAtt_, linePosAtt_,
        thickAtt_, zoomAtt_, centerXAtt_, centerYAtt_, fadeAtt_, blurAtt_, gammaAtt_, compAtt_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   colorAtt_;
    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoScrollPage)
};
