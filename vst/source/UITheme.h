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
