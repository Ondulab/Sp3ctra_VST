#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "SettingsWindow.h"
#include "CisVisualizerComponent.h"
#include "image/SourcesTabComponent.h"
#include "image/LuxPitchTabComponent.h"
#include "image/LuxMaskTabComponent.h"
#include "image/LuxStralTabComponent.h"
#include "image/LuxSynthTabComponent.h"
#include "image/ScoreGenTabComponent.h"
#include "image/VisualizerMode.h"
#include "video/VideoScrollPage.h"
#include "sampler/SamplerPageComponent.h"
#include "sampler/SequencerPageComponent.h"
#include "ui/ChainRackComponent.h"
#include "ui/KeyboardRulerComponent.h"
#include "ui/EngineAudioPanels.h"
#include "ui/VideoMixerColumn.h"
#include "ui/ModuleCatalogComponent.h"
#include "ui/SplitterBar.h"
#include "ui/setup/SourceSetupPanel.h"
#include "ui/setup/PitchSetupPanel.h"
#include "ui/setup/MaskSetupPanel.h"
#include "ui/setup/LuxStralSetupPanel.h"
#include "ui/setup/LuxSynthSetupPanel.h"
#include "ui/setup/LuxWaveSetupPanel.h"
#include "ui/setup/SamplerSetupPanel.h"
#include "ui/setup/ScoreSetupPanel.h"
#include "UITheme.h"
#include "Sp3ctraLookAndFeel.h"

// ============================================================================
// GearButton — settings icon rendered as a yellow cogwheel.
// Self-contained: painting logic lives in the header to avoid a separate TU.
// ============================================================================
class GearButton : public juce::Button
{
public:
    GearButton() : juce::Button("settings") {}

    void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override
    {
        const auto b  = getLocalBounds().toFloat().reduced(3.f);
        const float cx = b.getCentreX();
        const float cy = b.getCentreY();
        const float r  = juce::jmin(b.getWidth(), b.getHeight()) * 0.5f;

        // Background
        const juce::Colour bg(0xff2a2a2a);
        g.setColour(isButtonDown ? bg.brighter(0.3f)
                  : isMouseOver  ? bg.brighter(0.12f)
                  :                bg);
        g.fillRoundedRectangle(b, 4.f);

        // Cogwheel (yellow)
        const juce::Colour gear = isButtonDown ? juce::Colour(0xffffe066)
                                : isMouseOver  ? juce::Colour(0xffffcc00)
                                :                juce::Colour(0xffc89600);
        g.setColour(gear);
        g.fillPath(makeGearPath(cx, cy, r * 0.82f, 8));

        // Centre hole — punched out with background colour
        g.setColour(bg);
        g.fillEllipse(cx - r * 0.23f, cy - r * 0.23f, r * 0.46f, r * 0.46f);
    }

private:
    static juce::Path makeGearPath(float cx, float cy, float r, int teeth)
    {
        const float outer = r;
        const float inner = r * 0.68f;
        const float arc   = juce::MathConstants<float>::twoPi / (float)(teeth * 2);
        const float half  = arc * 0.36f;
        juce::Path p;
        bool first = true;
        for (int i = 0; i < teeth * 2; ++i)
        {
            const float ri = (i % 2 == 0) ? outer : inner;
            const float a0 = arc * (float)i - half;
            const float a1 = arc * (float)i + half;
            if (first) { p.startNewSubPath(cx + ri * std::cos(a0), cy + ri * std::sin(a0)); first = false; }
            else         p.lineTo         (cx + ri * std::cos(a0), cy + ri * std::sin(a0));
            p.lineTo(cx + ri * std::cos(a1), cy + ri * std::sin(a1));
        }
        p.closeSubPath();
        return p;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GearButton)
};

// ============================================================================
// PanicButton — "All Notes Off" panic, rendered as a red "PANIC" label (the
// de-facto standard on MIDI hardware and plugins). Releases every held/stuck
// MIDI note across all synth engines. Self-contained (painting in the header,
// like GearButton).
// ============================================================================
class PanicButton : public juce::Button
{
public:
    PanicButton() : juce::Button("panic")
    {
        setTooltip("All Notes Off (panic) - release every held/stuck note");
    }

    void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override
    {
        const auto b = getLocalBounds().toFloat().reduced(2.f);

        // Background
        const juce::Colour bg(0xff2a2a2a);
        g.setColour(isButtonDown ? bg.brighter(0.3f)
                  : isMouseOver  ? bg.brighter(0.12f)
                  :                bg);
        g.fillRoundedRectangle(b, 4.f);

        // Red outline + "PANIC" label
        const juce::Colour red = isButtonDown ? juce::Colour(0xffff6b6b)
                               : isMouseOver  ? juce::Colour(0xffff4d4d)
                               :                juce::Colour(0xffd83a3a);
        g.setColour(red.withAlpha(isMouseOver || isButtonDown ? 0.9f : 0.6f));
        g.drawRoundedRectangle(b, 4.f, 1.2f);

        g.setColour(red);
        g.setFont(juce::Font(juce::jmin(b.getHeight() * 0.46f, 15.f),
                             juce::Font::bold));
        g.drawText("PANIC", getLocalBounds(), juce::Justification::centred, false);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PanicButton)
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
        drawSegment(g, segmentBounds(true),  "SETUP",  setupFace, mouse);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (!e.mouseWasClicked())
            return;
        if (segmentBounds(false).contains(e.getPosition()))
            setFace(false, true);
        else if (segmentBounds(true).contains(e.getPosition()))
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
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontTiny)).boldened());
        g.drawText(text, r, juce::Justification::centred, false);
    }

    bool setupFace { false };
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
                                    public juce::DragAndDropContainer
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
    static constexpr int kHeaderH   = 52;
    static constexpr int kVisY      = kHeaderH + 8;
    // ZONE 1 stacks one panel per active visualizer output; its total height
    // grows with the panel count so each stays readable.
    static constexpr int kVisPanelH = 60;   // per-panel height
    static constexpr int kRulerH    = KeyboardRulerComponent::kPreferredH; // 26 (M5)

    /** Current ZONE 1 height = one row per active visualizer panel. */
    int visHeight() const noexcept { return juce::jmax(1, visPanelCount_) * kVisPanelH; }

    /** Top of zones 2/3/4 (below the visualizer strip), before the keyboard
     *  ruler offset. */
    int zonesBaseY() const noexcept { return kVisY + visHeight() + 6; }

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
    void openSettings();

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

    /** Writes editorW/H, zone2W/zone4W, scrollCollapsed into apvts.state. */
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
    int  luxStralEngineIndex_ { 0 };   // selected LuxStral engine (0 = A, 1 = B) — M8
    int zone2Width { kZone2DefaultW };
    int zone4Width { kZone4DefaultW };
    int splitterDragStartW { 0 };

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
    juce::Viewport  zone3Viewport;
    juce::Component zone3Content;

    // Hosted PLAY pages — children of zone3Content, one (or one stack) visible
    std::unique_ptr<SourcesTabComponent>  sourcesPage;
    std::unique_ptr<LuxPitchTabComponent> pitchPage;
    std::unique_ptr<LuxMaskTabComponent>  maskPage;
    std::unique_ptr<LuxStralTabComponent> imgLuxStralPage;
    std::unique_ptr<LuxSynthTabComponent> imgLuxSynthPage;
    std::unique_ptr<SamplerPageComponent> samplerPage;
    std::unique_ptr<SequencerPageComponent> sequencerPage;
    std::unique_ptr<ScoreGenTabComponent> scorePage;
    std::unique_ptr<VideoScrollPage>      videoScrollPage;   // OUT > VIDEO SCROLL (per-instance)
    std::unique_ptr<AudioSynthPanel>      audioSynthPanel;
    std::unique_ptr<AudioWavePanel>       audioWavePanel;

    // Hosted SETUP faces (M5 — migrated gear-wheel settings, same APVTS IDs)
    std::unique_ptr<SourceSetupPanel>     sourceSetup;   // SP3CTRA — network/CIS config
    std::unique_ptr<PitchSetupPanel>      pitchSetup;
    std::unique_ptr<MaskSetupPanel>       maskSetup;
    std::unique_ptr<LuxStralSetupPanel>   stralSetup;
    std::unique_ptr<LuxSynthSetupPanel>   synthSetup;
    std::unique_ptr<LuxWaveSetupPanel>    waveSetup;
    std::unique_ptr<SamplerSetupPanel>    samplerSetup;
    std::unique_ptr<ScoreSetupPanel>      scoreSetup;

    // ── ZONE 4: video scroll column (collapsible, detachable window) ──────────
    std::unique_ptr<VideoMixerColumn> waterfallColumn;   // ZONE 4 — right-band VIDEO MIX

    // ZONE 5 — reserved (output / master / monitoring): collapsed strip h=0.

    // ── LookAndFeel (declared before all JUCE components that use it) ─────────
    Sp3ctraLookAndFeel sp3ctraLaf;

    // ── Header: gear settings button ──────────────────────────────────────────
    GearButton settingsButton;
    PanicButton panicButton;   // All Notes Off (panic)
    std::unique_ptr<SettingsWindow> settingsWindow;

    // Tooltip support (palette rail stub + existing component tooltips)
    juce::TooltipWindow tooltipWindow { this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Sp3ctraAudioProcessorEditor)
};
