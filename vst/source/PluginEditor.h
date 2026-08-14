#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "AboutDialog.h"
#include "CisVisualizerComponent.h"
#include "image/SourcesTabComponent.h"
#include "image/LuxPitchTabComponent.h"
#include "image/LuxMaskTabComponent.h"
#include "image/LuxReverbTabComponent.h"
#include "image/LuxEchoTabComponent.h"
#include "image/LuxEqTabComponent.h"
#include "image/LuxHarmoTabComponent.h"
#include "image/LuxCentroTabComponent.h"
#include "image/LuxDriveTabComponent.h"
#include "image/LuxDcBlockTabComponent.h"
#include "image/LuxStralTabComponent.h"
#include "image/LuxSynthTabComponent.h"
#include "image/ScoreGenTabComponent.h"
#include "image/TimbreGenTabComponent.h"
#include "image/MidiScoreGenTabComponent.h"
#include "image/VoiceGenTabComponent.h"
#include "image/VisualizerMode.h"
#include "video/VideoScrollPage.h"
#include "midi/MidiTapPage.h"
#include "midi/MidiLearnAttachment.h"
#include "sampler/SamplerPageComponent.h"
#include "ui/ChainRackComponent.h"
#include "ui/KeyboardRulerComponent.h"
#include "ui/EngineAudioPanels.h"
#include "ui/LuxGrainPanel.h"
#include "ui/SynthOutPageComponent.h"
#include "ui/AudioMixPanel.h"
#include "ui/MidiMixPanel.h"
#include "ui/VideoMixerColumn.h"
#include "ui/ModuleCatalogComponent.h"
#include "ui/SplitterBar.h"
#include "ui/setup/SourceSetupPanel.h"
#include "sources/ui/MediaSourcePage.h"        // M9 — IMAGE/VIDEO/CAMERA PLAY faces
#include "ui/setup/PitchSetupPanel.h"
#include "ui/setup/MaskSetupPanel.h"
#include "ui/setup/LuxStralSetupPanel.h"
#include "ui/setup/LuxSynthSetupPanel.h"
#include "ui/setup/LuxWaveSetupPanel.h"
#include "ui/setup/LuxGrainSetupPanel.h"
#include "ui/setup/SamplerSetupPanel.h"
#include "ui/setup/ScoreSetupPanel.h"
#include "ui/setup/MidiScoreSetupPanel.h"
#include "ui/setup/TimbreSetupPanel.h"
#include "ui/setup/VoiceSetupPanel.h"
#include "ui/setup/VideoScrollSetupPanel.h"
#include "UITheme.h"
#include "Sp3ctraLookAndFeel.h"

// ============================================================================
// HeaderMenuButton — flat menu-bar item for the top banner (SESSION / MIDI /
// ADVANCED / ABOUT). Text label + optional coloured status dot (the SESSION
// item shows the active session name and its saved/unsaved dot). Hover pill
// highlight; the attached juce::PopupMenu anchors below the button.
// Self-contained painting (same idiom as the former GearButton).
// ============================================================================
class HeaderMenuButton : public juce::Button
{
public:
    explicit HeaderMenuButton(const juce::String& text)
        : juce::Button(text), label_(text) {}

    void setLabel(const juce::String& text)
    {
        if (label_ == text) return;
        label_ = text;
        repaint();
    }
    const juce::String& label() const noexcept { return label_; }

    /** Show/hide the status dot (SESSION saved/unsaved indicator). */
    void setDot(bool show, juce::Colour c)
    {
        dotVisible_ = show; dotColour_ = c; repaint();
    }

    static juce::Font font() { return juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSmall)).boldened(); }

    /** Width the button needs for its current label (+dot), incl. padding. */
    int idealWidth() const
    {
        return juce::GlyphArrangement::getStringWidthInt(font(), label_)
             + 2 * kHPadPx + (dotVisible_ ? kDotSpanPx : 0);
    }

    void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override
    {
        const auto b = getLocalBounds().toFloat().reduced(1.f);
        if (isMouseOver || isButtonDown)
        {
            g.setColour(juce::Colour(0xff3a3a3a).brighter(isButtonDown ? 0.15f : 0.f));
            g.fillRoundedRectangle(b, 4.f);
        }
        g.setColour(isMouseOver ? juce::Colours::white : juce::Colour(0xffc8cdd6));
        g.setFont(font());
        const int textW = getWidth() - 2 * kHPadPx - (dotVisible_ ? kDotSpanPx : 0);
        g.drawText(label_, kHPadPx, 0, textW, getHeight(),
                   juce::Justification::centredLeft, true);
        if (dotVisible_)
        {
            g.setColour(dotColour_);
            const float cy = getHeight() * 0.5f;
            g.fillEllipse((float) (kHPadPx + textW + 4), cy - 3.f, 6.f, 6.f);
        }
    }

private:
    static constexpr int kHPadPx   = 10;   // text side padding
    static constexpr int kDotSpanPx = 12;  // dot + gap

    juce::String label_;
    bool         dotVisible_ = false;
    juce::Colour dotColour_  = juce::Colours::green;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HeaderMenuButton)
};

// ============================================================================
// RailToggleButton — collapse / expand control for the far-left module
// catalogue rail (mirrors ZONE 4's collapse grip). Draws ✕ to collapse the
// rail and ▶ (pointing right, into where the rail re-appears) to expand it.
// Self-contained painting, like GearButton.
// ============================================================================
class RailToggleButton : public juce::Button
{
public:
    enum class Glyph { Collapse, Expand };

    explicit RailToggleButton(Glyph g) : juce::Button("railToggle"), glyph(g) {}

    void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override
    {
        const auto b = getLocalBounds().toFloat().reduced(1.f);

        const juce::Colour bg(0xff222230);
        g.setColour(isButtonDown ? bg.brighter(0.30f)
                  : isMouseOver  ? bg.brighter(0.12f)
                  :                bg);
        g.fillRoundedRectangle(b, 3.f);

        g.setColour(isMouseOver ? juce::Colour(0xffe8eef8) : juce::Colour(0xff9aa6ba));
        const auto inner = b.reduced(4.5f);   // matches ZONE 4's MiniButton exactly

        if (glyph == Glyph::Collapse)   // ✕
        {
            juce::Path p;
            p.startNewSubPath(inner.getX(),     inner.getY());
            p.lineTo         (inner.getRight(), inner.getBottom());
            p.startNewSubPath(inner.getRight(), inner.getY());
            p.lineTo         (inner.getX(),     inner.getBottom());
            g.strokePath(p, juce::PathStrokeType(1.6f));
        }
        else                            // ▶ — points right, into the rail
        {
            juce::Path p;
            p.addTriangle(inner.getX(),     inner.getY(),
                          inner.getX(),     inner.getBottom(),
                          inner.getRight(), inner.getCentreY());
            g.fillPath(p);
        }
    }

private:
    Glyph glyph;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RailToggleButton)
};

// ============================================================================
// ModulePowerButton — power switch for the selected module, sitting at the
// right end of the zone-3 PLAY|SETUP header row. A real toggle Button (so an
// APVTS ButtonAttachment keeps it in sync with the rack LED + host automation).
// Draws the universal power glyph, lit in the block's accent colour when on.
// ============================================================================
class ModulePowerButton : public juce::Button
{
public:
    ModulePowerButton() : juce::Button("modulePower")
    {
        setClickingTogglesState(true);
        setTooltip("Enable / disable this module");
    }

    void setAccent(juce::Colour c) { if (accent != c) { accent = c; repaint(); } }

    void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override
    {
        const auto b   = getLocalBounds().toFloat().reduced(2.f);
        const bool on  = getToggleState();

        const juce::Colour bg(0xff222836);
        g.setColour(isButtonDown ? bg.brighter(0.30f)
                  : isMouseOver  ? bg.brighter(0.12f)
                  :                bg);
        g.fillRoundedRectangle(b, 3.f);
        g.setColour(on ? accent.withAlpha(0.90f) : juce::Colour(0xff3a4250));
        g.drawRoundedRectangle(b, 3.f, on ? 1.4f : 1.f);

        // Power glyph: open ring (gap at top) + vertical stem through the gap.
        const float cx = b.getCentreX();
        const float cy = b.getCentreY() + 0.5f;
        const float r  = juce::jmin(b.getWidth(), b.getHeight()) * 0.26f;
        g.setColour(on ? accent.brighter(0.20f) : juce::Colour(0xff6b7280));

        juce::Path ring;
        ring.addCentredArc(cx, cy, r, r, 0.f,
                           juce::MathConstants<float>::pi * 0.30f,
                           juce::MathConstants<float>::pi * 1.70f, true);
        g.strokePath(ring, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
        g.drawLine(cx, cy - r - 1.5f, cx, cy - 0.5f, 1.6f);
    }

private:
    juce::Colour accent { juce::Colour(0xff4fa3e0) };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModulePowerButton)
};

// ============================================================================
// FaceSwitchBar — slim PLAY | SETUP switcher above the zone-3 block editor.
// Segmented look: the active face is highlighted with the selected block's
// accent colour. Self-contained (painting in the header, like GearButton).
// ============================================================================
class FaceSwitchBar : public juce::Component
{
public:
    FaceSwitchBar() { setRepaintsOnMouseActivity(true); }

    /** Fired when the user clicks the non-active segment. */
    std::function<void(bool setupFace)> onFaceChanged;

    void setFace(bool setupFaceIn, bool notify)
    {
        if (setupFace != setupFaceIn)
        {
            setupFace = setupFaceIn;
            repaint();
            if (notify && onFaceChanged)
                onFaceChanged(setupFace);
        }
    }

    bool isSetupFace() const noexcept { return setupFace; }

    /** Play-only mode: the block has no SETUP face — draw the single PLAY
     *  segment (always active) and ignore clicks on the SETUP area. */
    void setPlayOnly(bool po)
    {
        if (playOnly != po) { playOnly = po; repaint(); }
    }

    void setAccent(juce::Colour c)
    {
        if (accent != c) { accent = c; repaint(); }
    }

    void paint(juce::Graphics& g) override
    {
        // Bar background + bottom separator
        g.fillAll(juce::Colour(0xff1f1f28));
        g.setColour(juce::Colour(Sp3ctraTheme::kColBorder));
        g.fillRect(0, getHeight() - 1, getWidth(), 1);

        const auto mouse = getMouseXYRelative();
        drawSegment(g, segmentBounds(false), "PLAY",  !setupFace, mouse);
        if (!playOnly)
            drawSegment(g, segmentBounds(true), "SETUP", setupFace, mouse);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (!e.mouseWasClicked())
            return;
        if (segmentBounds(false).contains(e.getPosition()))
            setFace(false, true);
        else if (!playOnly && segmentBounds(true).contains(e.getPosition()))
            setFace(true, true);
    }

private:
    juce::Rectangle<int> segmentBounds(bool setupSegment) const
    {
        const int segW = 58;
        const int h    = getHeight() - 7;
        const int x0   = 8;
        return setupSegment ? juce::Rectangle<int>(x0 + segW + 3, 3, segW, h)
                            : juce::Rectangle<int>(x0,            3, segW, h);
    }

    void drawSegment(juce::Graphics& g, juce::Rectangle<int> r,
                     const juce::String& text, bool active,
                     juce::Point<int> mouse) const
    {
        const auto rf = r.toFloat();
        const bool hovered = r.contains(mouse);

        if (active)
        {
            g.setColour(accent.withAlpha(0.22f));
            g.fillRoundedRectangle(rf, 3.f);
            g.setColour(accent.withAlpha(0.95f));
            g.drawRoundedRectangle(rf, 3.f, 1.2f);
        }
        else
        {
            g.setColour(hovered ? juce::Colour(0xff2c2c3a) : juce::Colour(0xff232330));
            g.fillRoundedRectangle(rf, 3.f);
            g.setColour(juce::Colour(0xff3a3a4a));
            g.drawRoundedRectangle(rf, 3.f, 1.f);
        }

        g.setColour(active ? juce::Colours::white
                  : hovered ? juce::Colour(0xffb8c0d0)
                  :           juce::Colour(0xff8890a0));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontTab)).boldened());
        g.drawText(text, r, juce::Justification::centred, false);
    }

    bool setupFace { false };
    bool playOnly  { false };
    juce::Colour accent { juce::Colour(0xff4fa3e0) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FaceSwitchBar)
};

//==============================================================================
/**
 * @brief Main VST editor — M4 four-zone shell (resizable, no tab bar).
 *
 *   ┌────────────────────────────────────────────────────────────────┐
 *   │ Header (logo / version / ⚙ settings)                           │
 *   ├────────────────────────────────────────────────────────────────┤
 *   │ ZONE 1 — CisVisualizerComponent (full width, selection-driven) │
 *   ├──┬───────────┬──────────────────────────────┬──────────────────┤
 *   │▌P│ ZONE 2    │ ZONE 3 — block editor        │ ZONE 4           │
 *   │▌A│ chain     │ (vertical viewport hosting   │ video scroll     │
 *   │▌L│ rack      │  the selected block's page)  │ column           │
 *   │▌ │ (scroll↕) │                              │ (collapsible)    │
 *   └──┴───────────┴──────────────────────────────┴──────────────────┘
 *
 * Selection model: ChainRackComponent fires onBlockSelected(ChainBlockId);
 * selectBlock() then (1) highlights the rack block, (2) swaps the hosted
 * page(s) in zone 3, (3) switches the zone 1 visualizer source.
 * Pipeline-node clicks inside hosted pages may still override the
 * visualizer source ("last click wins").
 *
 * M5: zone 3 has two faces per block — PLAY (the M4 pages) and SETUP
 * (per-block settings migrated from the gear-wheel window, same APVTS IDs).
 * A slim FaceSwitchBar above the zone-3 viewport toggles between them; every
 * block now has a SETUP face (the SP3CTRA source exposes the network/CIS
 * config there). Selecting another block always resets to PLAY.
 *
 * Layout persistence (APVTS state ValueTree properties, survive reload):
 *   "editorW"/"editorH"  — window size
 *   "zone2W"/"zone4W"    — splitter positions
 *   "scrollCollapsed"    — zone 4 collapse state
 */
class Sp3ctraAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    public juce::DragAndDropContainer,
                                    private juce::Timer,
                                    private juce::ChangeListener
{
public:
    Sp3ctraAudioProcessorEditor(Sp3ctraAudioProcessor&);
    ~Sp3ctraAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    void suspendVisualizer();
    void resumeVisualizer();

    /** juce::DragAndDropContainer — internal module drags never export files. */
    bool shouldDropFilesWhenDraggedExternally(
        const juce::DragAndDropTarget::SourceDetails&,
        juce::StringArray&, bool&) override { return false; }

private:
    // ── Layout constants ──────────────────────────────────────────────────────
    // Single-row header: logo (left) + right-aligned menu bar (SESSION / MIDI /
    // ADVANCED / ABOUT). The SESSION menu item is Standalone-only.
    static constexpr int kTitleRowH = 44;

    int headerH() const noexcept { return kTitleRowH; }
    int visY() const noexcept { return headerH() + 8; }

    // ZONE 1 stacks one panel per active visualizer output; its total height
    // grows with the panel count so each stays readable.
    static constexpr int kVisPanelH = 60;   // per-panel height
    static constexpr int kRulerH    = KeyboardRulerComponent::kPreferredH; // 26 (M5)

    /** Current ZONE 1 height = one row per active visualizer panel. */
    int visHeight() const noexcept { return juce::jmax(1, visPanelCount_) * kVisPanelH; }

    /** Top of zones 2/3/4 (below the visualizer strip), before the keyboard
     *  ruler offset. */
    int zonesBaseY() const noexcept { return visY() + visHeight() + 6; }

    static constexpr int kPaletteW   = ModuleCatalogComponent::kRailW;  // module catalogue rail
    static constexpr int kCatHeaderH = 22;   // catalogue rail header band (title + ✕)
    static constexpr int kCatGripW   = 24;   // collapsed catalogue grip width
    static constexpr int kSplitterW = 6;
    static constexpr int kStackGap  = 12;    // gap between stacked zone-3 pages
    static constexpr int kFaceBarH  = 24;    // PLAY | SETUP switcher height

    // Zone width limits (spec §8) + defaults
    static constexpr int kZone2MinW     = 160;
    static constexpr int kZone3MinW     = 480;
    static constexpr int kZone4MinW     = 220;
    static constexpr int kZone2DefaultW = 200;
    static constexpr int kZone4DefaultW = 300;

    // Default / limit window sizes (spec §1)
    static constexpr int kDefaultW = 1280, kDefaultH = 820;
    static constexpr int kMinW = 1024, kMinH = 700, kMaxW = 4096, kMaxH = 2400;

    static constexpr int kHPad = Sp3ctraTheme::kHPad;

    // ── Behaviour ─────────────────────────────────────────────────────────────
    /** MIDI-follow — polls the mapping engine (~20 Hz) and, when enabled, jumps
     *  to the module a MIDI controller just moved. */
    void timerCallback() override;
    bool midiFollowEnabled() const;
    void followMidiParam(const juce::String& paramId);

    /** Single selection model — drives zones 1 + 2 + 3. */
    void selectBlock(ChainBlockId id);
    /** Contextual top-bandeau panels for LUXSTRAL: GRAY always, COLOR only when
     *  Stereo is on, BLOB only when StrokeForge is on. */
    std::vector<VisualizerMode> luxStralVisualizerSources() const;
    /** Re-apply the visualizer source list + Zone-1 height for the current block
     *  (used when a LUXSTRAL toggle flips a contextual panel on/off). */
    void refreshVisualizerSources();

    /** True if the block exposes a SETUP face (every block does — the SP3CTRA
     *  source hosts the network/CIS config there). */
    static bool blockHasSetup(ChainBlockId id) noexcept;

    /** Shows/hides zone-3 PLAY pages vs SETUP panels for the current
     *  selection + face. */
    void applyZone3Visibility();

    /** Collapses / expands the far-left module catalogue rail. Collapsing locks
     *  the chain rack (no module/chain deletion, reorder still allowed) and the
     *  freed width flows to the block editor; the state persists across reloads. */
    void setCatalogCollapsed(bool shouldCollapse, bool persist = true);

    /** Lays out zones 2/3/4 below the visualizer strip. */
    void layoutZones();

    /** Sizes zone3Content + positions the visible page(s) inside it. */
    void layoutZone3();

    /** Writes editorW/H, zone2W/zone4W, scrollCollapsed, catalogCollapsed and
     *  the zone-3 selection (block, PLAY/SETUP face, engine + video-slot
     *  bindings) into apvts.state so they ride in the session blob. */
    void persistLayoutProps();

    /** Top of the zones row — shifted down by the keyboard ruler when the
     *  selected block is PITCH or MASK (M5). Zone 1 height is unchanged. */
    int zonesTopY() const noexcept
    {
        return zonesBaseY() + (keyboardRuler != nullptr && keyboardRuler->isVisible()
                              ? kRulerH : 0);
    }

    Sp3ctraAudioProcessor& audioProcessor;

    // ── Selection / splitter state ────────────────────────────────────────────
    ChainBlockId selectedBlock { ChainBlockId::Chain1Source };
    int  visPanelCount_ { 1 };         // ZONE 1 stacked-panel count (drives its height)
    bool setupFace { false };          // false = PLAY, true = SETUP (per M5)
    // Synth-split P2 — for the synth blocks, WHICH view zone 3 hosts:
    // false = OUT page (per-chain send conditioning, reached from the rack),
    // true  = ENGINE page (reached from the ZONE-5 synth dock card).
    bool engineView_ { false };
    int  luxStralSendSlot_ { 0 };   // selected LuxStral SEND slot (0..7, OUT bank)
    int  samplerEngineIndex_  { 0 };   // selected Sampler engine (0 = A, 1 = B)
    int  videoSlotIndex_      { 0 };   // selected VideoScroll instance slot (0..7)
    int  midiTapSlotIndex_    { 0 };   // selected MIDI TAP instance slot (0..7)
    // zone2Width/zone4Width hold the USER INTENT (persisted in the session);
    // zone2Eff_/zone4Eff_ are what layoutZones() actually displayed after
    // clamping to the current window width. Keeping them separate stops a
    // narrow-window launch from permanently shrinking the saved widths — the
    // clamp used to write straight back into the persisted members.
    int zone2Width { kZone2DefaultW };
    int zone4Width { kZone4DefaultW };
    int zone2Eff_  { kZone2DefaultW };
    int zone4Eff_  { kZone4DefaultW };
    int splitterDragStartW { 0 };
    /** False until the ctor has read the persisted layout AND set the initial
     *  size — persistLayoutProps() is a no-op before that (see the ctor). */
    bool layoutRestoreDone_ { false };

    // ── ZONE 1: CIS Visualizer ────────────────────────────────────────────────
    std::unique_ptr<CisVisualizerComponent> cisVisualizer;

    // ── Keyboard ruler strip under zone 1 (M5 — PITCH / MASK only) ───────────
    std::unique_ptr<KeyboardRulerComponent> keyboardRuler;

    // ── Module catalogue rail (M6 — drag source for the chain rack) ───────────
    // Collapsible like ZONE 4: when collapsed it shrinks to a thin grip and the
    // chain rack is locked (delete affordances off, reorder still allowed).
    juce::Viewport         catalogViewport;
    ModuleCatalogComponent moduleCatalog;
    bool             catalogCollapsed { false };
    RailToggleButton catalogCollapseBtn { RailToggleButton::Glyph::Collapse };
    RailToggleButton catalogExpandBtn   { RailToggleButton::Glyph::Expand };

    // ── ZONE 2: chain rack (in a vertical viewport) ───────────────────────────
    juce::Viewport rackViewport;
    std::unique_ptr<ChainRackComponent> chainRack;

    // ── Splitters ─────────────────────────────────────────────────────────────
    SplitterBar splitterLeft;    // zone2 | zone3
    SplitterBar splitterRight;   // zone3 | zone4

    // ── ZONE 3: block editor host (vertical viewport + content container) ─────
    FaceSwitchBar   faceSwitch;        // PLAY | SETUP (every block has a SETUP face)
    ModulePowerButton modulePowerButton; // power switch at the right of the face row
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> modulePowerAttachment;
    std::unique_ptr<MidiLearnAttachment> modulePowerLearn; // right-click MIDI learn on the enable
    juce::Viewport  zone3Viewport;
    juce::Component zone3Content;

    // Hosted PLAY pages — children of zone3Content, one (or one stack) visible
    std::unique_ptr<SourcesTabComponent>  sourcesPage;
    std::unique_ptr<LuxPitchTabComponent> pitchPage;
    std::unique_ptr<LuxMaskTabComponent>  maskPage;
    std::unique_ptr<LuxStralTabComponent> imgLuxStralPage;
    std::unique_ptr<LuxSynthTabComponent> imgLuxSynthPage;
    std::unique_ptr<SamplerPageComponent> samplerPage;
    std::unique_ptr<ScoreGenTabComponent> scorePage;
    std::unique_ptr<TimbreGenTabComponent> timbrePage;      // UTILS > TIMBRE
    std::unique_ptr<MidiScoreGenTabComponent> midiScorePage; // UTILS > MIDI SCORE
    std::unique_ptr<VoiceGenTabComponent> voicePage;         // UTILS > VOICE (TTS)
    std::unique_ptr<LuxReverbTabComponent> reverbPage;       // FX > REVERB
    std::unique_ptr<LuxEchoTabComponent>   echoPage;         // FX > ECHO
    std::unique_ptr<LuxEqTabComponent>     eqPage;           // FX > EQ
    std::unique_ptr<LuxHarmoTabComponent>  harmoPage;        // FX > SCALE
    std::unique_ptr<LuxCentroTabComponent> centroPage;       // FX > CENTROID
    std::unique_ptr<LuxDriveTabComponent> drivePage;         // FX > LEVELS
    std::unique_ptr<LuxDcBlockTabComponent> dcBlockPage;     // FX > DC BLOCK
    std::unique_ptr<VideoScrollPage>      videoScrollPage;   // OUT > VIDEO SCROLL (per-instance)
    std::unique_ptr<MidiTapPage>          midiTapPage;       // OUT > MIDI TAP (per-instance)
    std::unique_ptr<AudioWavePanel>       audioWavePanel;
    std::unique_ptr<LuxGrainPanel>        luxGrainPanel;    // LUXGRAIN engine page (M4)
    std::unique_ptr<SynthOutPageComponent> synthOutPage;   // OUT/send page (synth-split P2)
    std::unique_ptr<MediaSourcePage>      imageSrcPage;      // M9 — SRC > IMAGE
    std::unique_ptr<MediaSourcePage>      videoSrcPage;      // M9 — SRC > VIDEO
    std::unique_ptr<MediaSourcePage>      cameraSrcPage;     // M9 — SRC > CAMERA

    // Hosted SETUP faces (M5 — migrated gear-wheel settings, same APVTS IDs)
    std::unique_ptr<SourceSetupPanel>     sourceSetup;   // SP3CTRA — network/CIS config
    std::unique_ptr<PitchSetupPanel>      pitchSetup;
    std::unique_ptr<MaskSetupPanel>       maskSetup;
    std::unique_ptr<LuxStralSetupPanel>   stralSetup;
    std::unique_ptr<LuxSynthSetupPanel>   synthSetup;
    std::unique_ptr<LuxWaveSetupPanel>    waveSetup;
    std::unique_ptr<LuxGrainSetupPanel>   grainSetup;
    std::unique_ptr<SamplerSetupPanel>    samplerSetup;
    std::unique_ptr<ScoreSetupPanel>      scoreSetup;
    std::unique_ptr<MidiScoreSetupPanel>  midiScoreSetup;  // export prefs (PNG/JPEG, A4/A3/FULL, DPI)
    std::unique_ptr<TimbreSetupPanel>     timbreSetup;     // export prefs (PNG/JPEG, DPI)
    std::unique_ptr<VoiceSetupPanel>      voiceSetup;      // export prefs (PNG/JPEG, A4/A3/Selection, DPI)
    std::unique_ptr<VideoScrollSetupPanel> videoScrollSetup;  // OUT > VIDEO SCROLL bg (per-instance)
    // (M9 media modules have no SETUP face — picking lives on MediaSourcePage)

    // ── ZONE 4: video scroll column (collapsible, detachable window) ──────────
    std::unique_ptr<VideoMixerColumn> waterfallColumn;   // ZONE 4 — right-band VIDEO MIX

    // ── AUDIO MIX — bottom half of ZONE 4 (P2b): the three global engines +
    // MASTER as vertical faders with VU meters. In the collapsed ZONE-4 band
    // it shrinks to a bare vertical MASTER fader (mini mode).
    std::unique_ptr<AudioMixPanel> audioMixPanel;
    std::unique_ptr<MidiMixPanel>  midiMixPanel;   // shown only when a probe is patched

    // ── LookAndFeel (declared before all JUCE components that use it) ─────────
    Sp3ctraLookAndFeel sp3ctraLaf;

    // ── Header menu bar (right-aligned) ───────────────────────────────────────
    // SESSION (Standalone only — shows the active session name + saved dot),
    // MIDI (follow toggle / clear mappings / panic), ADVANCED (log level /
    // worker threads), ABOUT (dialog + Ondulab links).
    HeaderMenuButton menuSessionBtn_  { "SESSION" };
    HeaderMenuButton menuMidiBtn_     { "MIDI" };
    HeaderMenuButton menuAdvancedBtn_ { "ADVANCED" };
    HeaderMenuButton menuAboutBtn_    { "ABOUT" };
    void layoutHeaderMenus();          // right-aligned row (widths follow labels)
    void showSessionMenu();
    void showMidiMenu();
    void showAdvancedMenu();
    void showAboutMenu();

    /** AppUpdater state changes (the only ChangeBroadcaster we listen to):
     *  light the ABOUT dot while an update is available / staged. */
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void refreshUpdateBadge();
    void exportMidiMappingsFlow();     // MIDI table → .sp3midi (reusable asset)
    void importMidiMappingsFlow();     // .sp3midi → MIDI table (replace, confirm)

    std::unique_ptr<juce::FileChooser> sessionChooser;
    juce::String shownSessionLabel_;   // change-detect for cheap header repaints
    /** Directory-picker → name-input → SessionManager action (NEW / SAVE AS). */
    void runSessionCreateFlow(bool saveAs);
    void refreshSessionBar();          // SESSION menu label + dot + relayout

    // Tooltip support (palette rail stub + existing component tooltips)
    juce::TooltipWindow tooltipWindow { this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Sp3ctraAudioProcessorEditor)
};
