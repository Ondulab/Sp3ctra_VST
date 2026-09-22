#pragma once

#include <cstdint> // uint32_t

/**
 * @file UITheme.h
 * @brief Sp3ctra VST — UI design tokens (single source of truth).
 *
 * Every UI component MUST reference these constants.
 * No magic numbers are allowed in layout or paint code.
 *
 * Hierarchy (from largest to smallest):
 *   kFontTitle     22 px — main application title
 *   kFontWindowTitle 18 px — floating window / dialog title
 *   kFontSection   16 px — settings-page section heading
 *   kFontSettings  14 px — label next to a control in settings tabs
 *   kFontBadge     12 px — section badge / component header strip
 *   kFontSmall     11 px — auxiliary labels (editor row labels, slot editor)
 *   kFontTiny      10 px — transport bar captions, sequencer hint text
 *   kFontMicro      9 px — smallest legible text (note names in slot cells)
 */

namespace Sp3ctraTheme
{
    // ──────────────────────────────────────────────────────────────────────────
    // CONTROL HEIGHTS  (buttons, combo-boxes, sliders, text-boxes)
    // ──────────────────────────────────────────────────────────────────────────

    /// Unified height for ALL interactive controls (button / slider / combobox).
    constexpr int kControlH    = 22;

    /// Height of a slider text-box sub-component — always equals kControlH.
    constexpr int kTextBoxH    = kControlH; // 28

    /// Square icon-button side length (Play / Stop in transport bar).
    constexpr int kIconBtnSize = kControlH; // 28

    /// Tab navigation button height (slightly shorter than standard controls).
    constexpr int kTabBtnH     = 22;

    /// Vertical bar faders (AUDIO MIX): the breathing room the LookAndFeel
    /// keeps above and below the bar inside the slider bounds — its "thumb
    /// radius", though the bar has no round thumb. JUCE shrinks the slider's
    /// bar/region by it, so anything laid out AGAINST the bar (the glued VU,
    /// the VIDEO ceiling overlay) derives its rectangle from this constant;
    /// a rect taken from the slider bounds instead overhangs the bar by this
    /// much and paints over its outline.
    constexpr int kVFaderInset = 2;

    // ──────────────────────────────────────────────────────────────────────────
    // ROW / STEP METRICS
    // ──────────────────────────────────────────────────────────────────────────

    /// Vertical gap between consecutive control rows.
    constexpr int kRowGap  = 4;

    /// Full vertical row step = kControlH + kRowGap.
    constexpr int kRowStep = kControlH + kRowGap; // 32

    // ──────────────────────────────────────────────────────────────────────────
    // PADDING & SPACING
    // ──────────────────────────────────────────────────────────────────────────

    /// Outer horizontal padding (editor left/right margin).
    constexpr int kHPad = 10;

    /// Inner component padding (between component edge and content).
    constexpr int kPad  = 4;

    /// Inter-element gap (horizontal space between sibling components).
    constexpr int kGap  = 6;

    /// Default label column width in a label + control two-column row.
    constexpr int kLabelW     = 110;

    /// Wide label column — for settings tabs with long parameter names (e.g. LuxStral).
    constexpr int kLabelWide  = 140;

    // ──────────────────────────────────────────────────────────────────────────
    // STRETCH LIMITS  (max widths — content stops following very wide zones)
    // ──────────────────────────────────────────────────────────────────────────

    /// Max width of ONE column of label+control rows (mix panels, settings
    /// columns, the chain rack). A wider host left-aligns the column and
    /// leaves the remainder empty — sliders/combos never stretch past this.
    constexpr int kMaxContentW = 560;

    /// Max width of a full ZONE-3 page (two kMaxContentW columns + gap —
    /// 2-column module pages and graphic editors stay usable, not absurd).
    constexpr int kMaxPageW = 2 * kMaxContentW + 16;

    // ──────────────────────────────────────────────────────────────────────────
    // SECTION / BADGE HEIGHTS
    // ──────────────────────────────────────────────────────────────────────────

    /// Coloured section badge / component header strip height.
    constexpr int kSectionH   = 24;

    /// Vertical gap below section badge before the first control row.
    constexpr int kSectionGap = 4;

    // ──────────────────────────────────────────────────────────────────────────
    // SLIDER TEXT-BOX WIDTHS  (all paired with kTextBoxH = 28)
    // ──────────────────────────────────────────────────────────────────────────

    /// Wide text-box — BPM and other long numeric values.
    constexpr int kTbWide    = 82;

    /// Standard text-box — most parameter sliders.
    constexpr int kTbStd     = 72;

    /// Extra-narrow text-box — short duration values ("12.3 s").
    constexpr int kTbXNarrow = 60;

    /// Narrow text-box — compact values (e.g. speed "1.00×").
    constexpr int kTbNarrow  = 52;

    // ──────────────────────────────────────────────────────────────────────────
    // FONT SIZES
    // ──────────────────────────────────────────────────────────────────────────

    /// Main application title ("Sp3ctra").
    constexpr float kFontTitle       = 22.0f;

    /// Floating window / dialog title ("Sp3ctra Settings").
    constexpr float kFontWindowTitle = 18.0f;

    /// Settings-page section heading ("General Configuration").
    constexpr float kFontSection     = 16.0f;

    /// Standard label next to a control in a settings tab.
    constexpr float kFontSettings    = 14.0f;

    /// Section badge / component header label ("LUXSTRAL", "STEP SEQUENCER").
    constexpr float kFontBadge       = 12.0f;

    /// Small auxiliary text (editor row labels, slot editor panel labels,
    /// popup-menu items, header nav buttons, range-info labels).
    constexpr float kFontSmall       = 14.0f;

    /// Tiny text (transport bar captions, hint text, log lines).
    constexpr float kFontTiny        = 13.0f;

    /// Micro text — smallest legible label (note names in slot cells).
    constexpr float kFontMicro       = 9.0f;

    /// Button label font — used by Sp3ctraLookAndFeel::drawButtonText.
    constexpr float kFontBtn         = 13.0f;

    // ──────────────────────────────────────────────────────────────────────────
    // COLOURS — dark theme
    // ──────────────────────────────────────────────────────────────────────────

    /// Main window background.
    constexpr uint32_t kColBg         = 0xff1e1e1e;

    /// Component panel background.
    constexpr uint32_t kColPanelBg    = 0xff282828;

    /// Dark inner-surface background (e.g. SlotEditor right panel).
    constexpr uint32_t kColSurface    = 0xff1a1a2a;

    /// Separator / border line colour.
    constexpr uint32_t kColBorder     = 0xff3a3a3a;

    /// Standard body text colour.
    constexpr uint32_t kColText       = 0xffb8c4d0;

    /// Muted / disabled text colour.
    constexpr uint32_t kColTextMuted  = 0xff888888;

    /// Default button background.
    constexpr uint32_t kColBtnBg      = 0xff2a2a2a;

    /// Active / highlighted button background.
    constexpr uint32_t kColBtnActive  = 0xff3a3a3a;

    // ──────────────────────────────────────────────────────────────────────────
    // HANDLE / CONTROL ACCENT  ("what you touch")
    // ──────────────────────────────────────────────────────────────────────────
    //
    // Every module page paints its DISPLAY (curves, frames, captions, box
    // labels, section captions) in the module's category colour — see
    // ModuleCatalog::moduleColour. Everything the user can GRAB is painted in
    // ONE vivid hue instead, deliberately outside the five category hues
    // (cyan / magenta / violet / amber / green) so a control pops on every
    // page: graphic-editor handles (nodes, chevrons, fader thumbs, grabbable
    // lines), bar sliders, toggles, type chips.
    //
    //   "what you look at" = module colour     "what you touch" = kColHandle
    //
    // Painting recipes live in ui/Sp3ctraHandles.h — never restate these
    // literals in an editor.

    /// THE handle / control colour (acid lime) — a control that is HOT:
    /// hovered, selected, or being edited (mouse, MIDI, automation).
    constexpr uint32_t kColHandle     = 0xffdcff3c;

    /// Neutral a resting control is pulled toward (kCtlRestMix of the way):
    /// at rest a page must read CALM, so an untouched handle / bar / combo
    /// wears a desaturated version of its accent instead of the full hue.
    /// Any accent (mixer identity tints included) desaturates the same way —
    /// see Sp3ctraControls::restOf().
    constexpr uint32_t kColCtlNeutral = 0xff7c8698;

    /// How far a resting control travels toward kColCtlNeutral (0 = full
    /// accent, 1 = plain grey). 0.42 keeps the family readable.
    constexpr float    kCtlRestMix    = 0.42f;

    /// Ring of a handle while it is being EDITED — from the mouse OR from a
    /// MIDI controller / automation (and the bar's drag edge).
    constexpr uint32_t kColHandleHot  = 0xffffffff;

    // — Interaction ladder: ONE alpha per rung, for handles AND widgets.
    //   Idle (rest colour) < Selected < Hover < Edit. Sp3ctraControls resolves
    //   them; no component restates an alpha.

    /// Resting control — not hovered, not selected, not being edited.
    constexpr float kCtlAlphaIdle  = 0.72f;

    /// Persistent selection (the EQ handle the boxes / CCs talk to).
    constexpr float kCtlAlphaSel   = 0.95f;

    /// Hovered / being edited — full accent.
    constexpr float kCtlAlphaHot   = 1.00f;

    /// Disabled control — present, clearly inert.
    constexpr float kCtlAlphaOff   = 0.26f;

    /// Halo alpha under a hovered / edited control.
    constexpr float kCtlHaloHover  = 0.22f;
    constexpr float kCtlHaloEdit   = 0.34f;

    /// Outline / ring widths along the ladder.
    constexpr float kCtlLineIdle   = 1.2f;
    constexpr float kCtlLineSel    = 1.4f;
    constexpr float kCtlLineHot    = 1.6f;
    constexpr float kCtlLineEdit   = 1.9f;

    /// Value-fill alphas of a bar / toggle track along the same ladder.
    constexpr float kCtlFillIdle   = 0.20f;
    constexpr float kCtlFillHot    = 0.34f;
    constexpr float kCtlFillEdit   = 0.44f;

    /// How long a control keeps glowing after a change it did NOT get from
    /// its own mouse drag (MIDI CC, automation, preset) — "what is being
    /// edited" stays visible for a moment after the move.
    constexpr double kCtlGlowMs    = 1500.0;

    /// Dark core of an IDLE handle — the lime ring reads on top of it. Same
    /// value as the graphic-frame fill so an idle node looks punched out.
    /// MODULATION — the identity of the LFO bank (midi/LfoBank.h): its
    /// pastilles, the "LFO 3" source token of a MIDI MAP row, the window a
    /// modulated control shows. Chosen OUTSIDE the five category hues (cyan
    /// SRC / magenta MIDI / violet FX / amber UTILS / green OUT) and outside
    /// the lime of controls: a modulated destination must not read as a
    /// module, and a source that moves on its own is not something you touch.
    constexpr uint32_t kColMod        = 0xff6f8cff;

    constexpr uint32_t kColHandleCore = 0xff20202a;

    /// Graphic-editor frame fill (the window every module editor draws its
    /// visualisation in) — the module outline sits on it at 25 %.
    constexpr uint32_t kColFrameBg    = 0xff20202a;

    /// Bar-slider interior (enabled / disabled).
    constexpr uint32_t kColBarBg      = 0xff181820;
    constexpr uint32_t kColBarBgOff   = 0xff121216;

    // ──────────────────────────────────────────────────────────────────────────
    // TAB DESIGN TOKENS  (main tabs + sub-tabs)
    // ──────────────────────────────────────────────────────────────────────────

    // — Main tab bar (IMAGE / SYNTH / SAMPLER) —

    /// Tab bar background strip.
    constexpr uint32_t kColTabBarBg         = 0xff1a1a1a;

    /// Active main tab background.
    constexpr uint32_t kColTabActiveBg      = 0xff2e2e38;

    /// Inactive main tab background.
    constexpr uint32_t kColTabInactiveBg    = 0xff1a1a1a;

    /// Active main tab text colour (full white).
    constexpr uint32_t kColTabActiveText    = 0xffeeeeee;

    /// Inactive main tab text colour (dimmed).
    constexpr uint32_t kColTabInactiveText  = 0xff686878;

    /// Active tab top/side border glow.
    constexpr uint32_t kColTabBorderActive  = 0xff4a4a5a;

    /// Inactive tab border (very subtle).
    constexpr uint32_t kColTabBorderInactive = 0xff2a2a2a;

    // — Sub-tab bar (SOURCES / LUXSTRAL / LUXSYNTH inside IMAGE page) —

    /// Sub-tab bar background.
    constexpr uint32_t kColSubTabBarBg        = 0xff12161e;

    /// Inactive sub-tab background.
    constexpr uint32_t kColSubTabInactiveBg   = 0xff12161e;

    /// Inactive sub-tab text.
    constexpr uint32_t kColSubTabInactiveText = 0xff586878;

    // — Tab geometry —

    /// Height of the accent underline for the active tab.
    constexpr int kTabUnderlineH    = 3;

    /// Corner radius for tab shapes.
    constexpr float kTabCornerR     = 4.0f;

    /// Primary face-switch tab labels (FaceSwitchBar "PLAY | SETUP").
    constexpr float kFontTab        = 14.0f;

    /// Sub-navigation labels (module-catalogue category headers SRC/SYNTH/UTILS).
    constexpr float kFontSubTab     = 13.0f;

} // namespace Sp3ctraTheme
