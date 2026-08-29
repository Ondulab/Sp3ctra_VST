#include "ChainRackComponent.h"
#include "../Sp3ctraCore.h"
#include "../sources/MediaSourceEngines.h"   // M9 — media source LEDs
#include "ChainPresetIO.h"                   // J4 — .sp3chain presets
#include "../Sp3ctraDialog.h"
#include "../licensing/ActivationDialog.h"

// C engine state — read-only here (LED monitoring)
extern "C" {
    #include "processing/lux_pitch.h"                 // g_lux_pitch_proc
    #include "processing/lux_mask.h"                  // g_lux_mask_proc
    #include "processing/lux_reverb.h"                // FX pools — LED monitoring
    #include "processing/lux_echo.h"
    #include "processing/lux_eq.h"
    #include "processing/lux_harmo.h"
    #include "processing/lux_centro.h"
    #include "processing/lux_drive.h"
    #include "processing/lux_dcblock.h"
    #include "processing/lux_gain.h"
    #include "processing/midi_tap.h"
    #include "audio/buffers/audio_image_buffers.h"    // lines_received counter
}

namespace
{
    // Chain group header colours (cycled per chain index)
    const juce::Colour kColChain1Hdr { 0xffe0b84a }; // amber
    const juce::Colour kColChain2Hdr { 0xff4ae0a0 }; // green
    const juce::Colour kColChain3Hdr { 0xffc0c4cc }; // grey
    const juce::Colour kColConnector { 0xff3a4250 };
    const juce::Colour kColGutter    { 0xff121218 }; // rack ground between chain cards
    const juce::Colour kColCard      { 0xff181820 }; // chain card body (the former rack bg)
    const juce::Colour kColMarkerTxt { 0xff6b7280 }; // IN / END captions

    juce::Colour chainHeaderColour(int idx)
    {
        switch (idx % 3)
        {
            case 0:  return kColChain1Hdr;
            case 1:  return kColChain2Hdr;
            default: return kColChain3Hdr;
        }
    }
}

//==============================================================================
// ChainBlockId ↔ ModuleType shim
//==============================================================================
ModuleType chainBlockToModuleType(ChainBlockId id) noexcept
{
    switch (id)
    {
        case ChainBlockId::Pitch:    return ModuleType::Pitch;
        case ChainBlockId::Mask:     return ModuleType::Mask;
        case ChainBlockId::Reverb:   return ModuleType::Reverb;
        case ChainBlockId::Echo:     return ModuleType::Echo;
        case ChainBlockId::Equalizer:return ModuleType::Equalizer;
        case ChainBlockId::Harmonize:return ModuleType::Harmonize;
        case ChainBlockId::Centroid: return ModuleType::Centroid;
        case ChainBlockId::Drive:    return ModuleType::Drive;
        case ChainBlockId::DcBlock:  return ModuleType::DcBlock;
        case ChainBlockId::Gain:     return ModuleType::Gain;
        case ChainBlockId::Sampler:  return ModuleType::Sampler;
        case ChainBlockId::Score:    return ModuleType::Score;
        case ChainBlockId::Timbre:   return ModuleType::Timbre;
        case ChainBlockId::MidiScore:return ModuleType::MidiScore;
        case ChainBlockId::Voice:    return ModuleType::Voice;
        case ChainBlockId::LuxStral: return ModuleType::LuxStral;
        case ChainBlockId::LuxSynth: return ModuleType::LuxSynth;
        case ChainBlockId::LuxWave:  return ModuleType::LuxWave;
        case ChainBlockId::LuxGrain: return ModuleType::LuxGrain;
        case ChainBlockId::VideoScroll: return ModuleType::VideoScroll;
        case ChainBlockId::MidiTap:   return ModuleType::MidiTap;
        case ChainBlockId::ImageSrc:  return ModuleType::Image;
        case ChainBlockId::VideoSrc:  return ModuleType::Video;
        case ChainBlockId::CameraSrc: return ModuleType::Camera;
        case ChainBlockId::Chain1Source:
        case ChainBlockId::Chain2Source:
        default:                     return ModuleType::Sp3ctra;
    }
}

juce::Colour ChainRackComponent::blockColour(ChainBlockId id) noexcept
{
    if (id == ChainBlockId::None)
        return juce::Colours::grey;
    return moduleColour(chainBlockToModuleType(id));
}

juce::String ChainRackComponent::enableParamId(ChainBlockId id) noexcept
{
    if (id == ChainBlockId::None)
        return {};
    return moduleEnableParam(chainBlockToModuleType(id));
}

//==============================================================================
// BlockComponent
//==============================================================================
juce::Rectangle<float> ChainRackComponent::BlockComponent::dotBounds() const
{
    const auto b = getLocalBounds().toFloat().reduced(2.f);
    const float r = 8.f;
    return { b.getRight() - 11.f - r, b.getCentreY() - r, 2 * r, 2 * r };
}

juce::Rectangle<float> ChainRackComponent::BlockComponent::closeBounds() const
{
    const auto b = getLocalBounds().toFloat().reduced(2.f);
    const float s = 13.f;
    return { b.getRight() - 11.f - 16.f - s, b.getCentreY() - s * 0.5f, s, s };
}

void ChainRackComponent::BlockComponent::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced(2.f);

    // Selection halo — deliberately loud: blocks of a category share one hue,
    // so the selected block must read at a glance among identical neighbours.
    if (selected)
    {
        g.setColour(colour.withAlpha(0.50f));
        g.drawRoundedRectangle(b.expanded(2.0f), 7.0f, 3.5f);
    }

    const juce::Colour bg = selected      ? colour.withAlpha(0.30f)
                          : isMouseOver() ? colour.withAlpha(0.10f)
                          :                 juce::Colour(0xff1a1f2a);
    g.setColour(bg);
    g.fillRoundedRectangle(b, 5.f);
    g.setColour(selected ? colour : colour.withAlpha(0.35f));
    g.drawRoundedRectangle(b, 5.f, selected ? 2.0f : 1.f);

    // ── State LED (right side): ● active / ◐ idle / ○ off ───────────────────
    {
        const float r  = 4.5f;
        const float cx = b.getRight() - 11.f;
        const float cy = b.getCentreY();
        const juce::Rectangle<float> dot(cx - r, cy - r, 2 * r, 2 * r);

        if (overDot && enableParam.isNotEmpty())
        {
            g.setColour(colour.withAlpha(0.45f));
            g.drawEllipse(dot.expanded(3.5f), 1.2f);
        }

        switch (led)
        {
            case LedState::Active:
                g.setColour(colour.withAlpha(0.30f));
                g.fillEllipse(dot.expanded(2.5f));
                g.setColour(colour.brighter(0.25f));
                g.fillEllipse(dot);
                break;

            case LedState::Idle:
            {
                g.setColour(colour.withAlpha(0.20f));
                g.fillEllipse(dot);
                juce::Path half;
                half.addPieSegment(dot, juce::MathConstants<float>::pi,
                                        juce::MathConstants<float>::twoPi, 0.f);
                g.setColour(colour.withAlpha(0.55f));
                g.fillPath(half);
                g.setColour(colour.withAlpha(0.55f));
                g.drawEllipse(dot, 1.f);
                break;
            }

            case LedState::Off:
            default:
                g.setColour(juce::Colour(0xff3a3f4a));
                g.drawEllipse(dot, 1.2f);
                break;
        }
    }

    // ── Remove (×) — only while hovered, and only when removable (rack unlocked)
    if (removable && isMouseOver())
    {
        const auto x = closeBounds();
        g.setColour(overClose ? juce::Colour(0xffe06b6b) : juce::Colour(0xff6b7280));
        const float pad = 3.5f;
        g.drawLine(x.getX() + pad, x.getY() + pad, x.getRight() - pad, x.getBottom() - pad, 1.4f);
        g.drawLine(x.getRight() - pad, x.getY() + pad, x.getX() + pad, x.getBottom() - pad, 1.4f);
    }

    // ── Name ──────────────────────────────────────────────────────────────────
    // A badge leads the label: tiny keyboard for MIDI-input modules, boxed "FX"
    // for the FX section — same glyphs as the catalogue chips.
    auto textArea = b.reduced(9.f, 0.f).withTrimmedRight(40.f);
    if (moduleNeedsMidi(type))
    {
        const float icoW = 12.f, icoH = 11.f;
        const juce::Rectangle<float> iconR(b.getX() + 8.f, b.getCentreY() - icoH * 0.5f,
                                           icoW, icoH);
        ModuleIcons::drawMidiKeyboard(g, iconR, colour.withAlpha(selected ? 0.95f : 0.62f));
        textArea = textArea.withLeft(iconR.getRight() + 6.f);
    }
    else if (moduleIsFx(type))
    {
        const float icoW = 14.f, icoH = 12.f;
        const juce::Rectangle<float> iconR(b.getX() + 8.f, b.getCentreY() - icoH * 0.5f,
                                           icoW, icoH);
        ModuleIcons::drawFxBadge(g, iconR, colour.withAlpha(selected ? 0.95f : 0.62f));
        textArea = textArea.withLeft(iconR.getRight() + 6.f);
    }

    g.setColour(selected ? juce::Colours::white : colour.brighter(0.3f));
    g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
    g.drawText(name,
               textArea.toNearestInt(),
               juce::Justification::centredLeft, true);
}

void ChainRackComponent::BlockComponent::mouseDown(const juce::MouseEvent&)
{
    dragging = false;
}

void ChainRackComponent::BlockComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (dragging || e.getDistanceFromDragStart() < 6)
        return;

    if (auto* dnd = juce::DragAndDropContainer::findParentDragContainerFor(this))
    {
        if (! dnd->isDragAndDropActive())
        {
            dragging = true;
            dnd->startDragging(ModuleDrag::fromRackMove(uid), this,
                               juce::ScaledImage(createComponentSnapshot(getLocalBounds())));
        }
    }
}

void ChainRackComponent::BlockComponent::mouseUp(const juce::MouseEvent& e)
{
    const bool wasDragging = dragging;
    dragging = false;
    if (wasDragging || ! e.mouseWasClicked())
        return;

    if (removable && closeBounds().contains(e.position) && onRemove)
        onRemove(uid);
    else if (enableParam.isNotEmpty() && dotBounds().contains(e.position) && onToggleEnable)
        onToggleEnable();
    else if (onClick)
        onClick(uid);
}

void ChainRackComponent::BlockComponent::mouseMove(const juce::MouseEvent& e)
{
    const bool od = enableParam.isNotEmpty() && dotBounds().contains(e.position);
    const bool oc = removable && closeBounds().contains(e.position);
    if (od != overDot || oc != overClose)
    {
        overDot   = od;
        overClose = oc;
        setMouseCursor((od || oc) ? juce::MouseCursor::PointingHandCursor
                                  : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void ChainRackComponent::BlockComponent::mouseExit(const juce::MouseEvent&)
{
    if (overDot || overClose)
    {
        overDot = overClose = false;
        setMouseCursor(juce::MouseCursor::NormalCursor);
        repaint();
    }
}

//==============================================================================
// ChainRackComponent
//==============================================================================
ChainRackComponent::ChainRackComponent(Sp3ctraAudioProcessor& p)
    : processor(p), model(p.getChainModel())   // model owned by the processor
{
    // The processor already loaded + validated the topology and derived routing
    // (constructor / setStateInformation). We just render it.
    rebuild();
    startTimerHz(10);   // LED refresh
}

ChainRackComponent::~ChainRackComponent()
{
    stopTimer();
}

//==============================================================================
// Build / mutate
//==============================================================================
void ChainRackComponent::rebuild()
{
    blocks.clear();

    for (int c = 0; c < model.numChains(); ++c)
    {
        for (auto& m : model.chains[(size_t) c].modules)
        {
            auto blk = std::make_unique<BlockComponent>(m.type, m.id);
            auto* bp = blk.get();
            // Synth-split M6: every engine send's LED is the PER-SEND power,
            // from the send's own conditioning bank — the ENGINE enables live
            // on the AUDIO MIX strips.
            if (m.type == ModuleType::LuxStral && m.slot >= 0)
                bp->setEnableParamOverride(lsOutParam(m.slot, "enabled"));
            if (m.type == ModuleType::LuxSynth && m.slot >= 0)
                bp->setEnableParamOverride(lxOutParam(m.slot, "enabled"));
            if (m.type == ModuleType::LuxWave && m.slot >= 0)
                bp->setEnableParamOverride(lwOutParam(m.slot, "enabled"));
            if (m.type == ModuleType::LuxGrain && m.slot >= 0)
                bp->setEnableParamOverride(lgOutParam(m.slot, "enabled"));
            // Each VideoScroll output is per-instance: its LED toggles the slot's
            // own enable param, so the mixer can drop just this output.
            if (m.type == ModuleType::VideoScroll && m.slot >= 0)
                bp->setEnableParamOverride(vsParam(m.slot, "enabled"));
            // Same per-instance rule for MIDI TAP (its own 8-slot pool).
            if (m.type == ModuleType::MidiTap && m.slot >= 0)
                bp->setEnableParamOverride(mtParam(m.slot, "enabled"));
            // P5-M3 media sources (Image/Video/Camera) are per-slot engines: the
            // rack LED must toggle THIS instance's own enable param, not the
            // global default (slot 0). Without this override every chain's LED
            // wrote "imgSrcEnabled" == slot 0, so Chain 2's button flipped
            // Chain 1's Image. The LED read side already uses mi->slot.
            if (m.type == ModuleType::Image && m.slot >= 0)
                bp->setEnableParamOverride(imgSrcParam(m.slot, "Enabled"));
            if (m.type == ModuleType::Video && m.slot >= 0)
                bp->setEnableParamOverride(vidSrcParam(m.slot, "Enabled"));
            if (m.type == ModuleType::Camera && m.slot >= 0)
                bp->setEnableParamOverride(camSrcParam(m.slot, "Enabled"));
            // P6 sampler engines are per-slot too: the LED toggles THIS engine's
            // own enable, so a Sampler in chain 2 no longer flips chain 1's.
            if (m.type == ModuleType::Sampler && m.slot >= 0)
                bp->setEnableParamOverride(fsEngineParam(m.slot, "Enabled"));
            // Pooled inserts are per-instance too: the LED toggles the enable of
            // THIS instance's bank (pool slot bound to the module UUID).
            if (m.type == ModuleType::Pitch || m.type == ModuleType::Mask
                || m.type == ModuleType::Reverb || m.type == ModuleType::Echo
                || m.type == ModuleType::Equalizer || m.type == ModuleType::Harmonize
                || m.type == ModuleType::Centroid || m.type == ModuleType::Drive
                || m.type == ModuleType::DcBlock || m.type == ModuleType::Gain)
                bp->setEnableParamOverride(insertBankParam(
                    m.type, processor.poolSlotForInstance(m.id), "Enabled"));
            // Score family (SCORE/TIMBRE/MIDI SCORE/VOICE): the LED is the
            // module ACTIVE toggle of ITS player slot — decoupled from the
            // transport (PLAY lives on the page). Deactivating stops the
            // reading (remembering the head); reactivating resumes it.
            if (isScoreFamily(m.type))
                bp->setEnableParamOverride(scoreActiveParam(m.slot >= 0 ? m.slot : 0));
            // SP3CTRA source: the LED is the module's PLAY/STOP switch — it
            // drives the global transport (imageFreezeMode, 0=play/1=hold/
            // 2=stop). toggleEnable's normalized flip maps exactly: playing
            // (0.0 < 0.5) → stop (1.0); paused (0.5) or stopped (1.0) → play.
            if (m.type == ModuleType::Sp3ctra)
                bp->setEnableParamOverride("imageFreezeMode");
            bp->onClick        = [this](juce::Uuid id) { selectInstance(id, true); };
            bp->onToggleEnable = [this, bp]            { toggleEnable(bp->getEnableParam()); };
            bp->onRemove       = [this](juce::Uuid id) { removeInstance(id); };
            bp->setRemovable(! locked);
            const bool hasLed = bp->getEnableParam().isNotEmpty();
            bp->setTooltip(hasLed
                ? (locked ? "Click the LED to enable/disable - drag to reorder"
                          : "Click the LED to enable/disable - drag to reorder - x to remove")
                : (locked ? "Drag to reorder" : "Drag to reorder - x to remove"));
            addAndMakeVisible(bp);
            blocks.push_back(std::move(blk));
        }
    }

    applyHighlight();
    updateLeds();
    if (getWidth() > 0)
        resized();
}

void ChainRackComponent::mutateAndRefresh(bool notifySelection)
{
    // The processor owns the model: apply the enable/routing bridge + persist.
    processor.onChainModelEdited();
    refreshAfterModelEdit(notifySelection);
}

void ChainRackComponent::refreshAfterModelEdit(bool notifySelection)
{
    rebuild();

    // Keep the selection valid; fall back to the first module if it vanished.
    int c = -1, i = -1;
    if (model.find(selectedId, c, i) == nullptr)
    {
        selectedId     = firstInstanceId();
        notifySelection = true;
    }
    applyHighlight();

    if (notifySelection && onBlockSelected)
    {
        int sc = -1, si = -1;
        if (auto* m = model.find(selectedId, sc, si))
        {
            if (m->type == ModuleType::VideoScroll && onVideoBlockSelected)
                onVideoBlockSelected(m->slot);
            if (m->type == ModuleType::Sampler && onSamplerBlockSelected)
                onSamplerBlockSelected(m->slot);
            if (m->type == ModuleType::LuxStral && onLuxStralBlockSelected)
                onLuxStralBlockSelected(m->slot);   // send slot (0..7, OUT bank)
            onBlockSelected(instanceToBlockId(m->type, sc));
        }
        else
        {
            // Rack is empty (every module was deleted): the editor must clear
            // zone 1 (visualizer) and zone 3 (pages) — a stale "last module"
            // view with nothing selected is a lie.
            onBlockSelected(ChainBlockId::None);
        }
    }

    if (onModelChanged)
        onModelChanged();   // editor re-runs layoutZones (preferred height changed)

    repaint();
}

void ChainRackComponent::toggleEnable(const juce::String& paramId)
{
    if (paramId.isEmpty())
        return;
    if (auto* param = processor.getAPVTS().getParameter(paramId))
    {
        const float newNorm = (param->getValue() < 0.5f) ? 1.0f : 0.0f;
        param->beginChangeGesture();
        param->setValueNotifyingHost(newNorm);
        param->endChangeGesture();
    }
    updateLeds();
}

void ChainRackComponent::removeInstance(const juce::Uuid& id)
{
    int c = -1, i = -1;
    const auto* inst = model.find(id, c, i);
    if (inst == nullptr)
        return;

    // No confirmation: the module's settings memory survives (typeMemory) and
    // it can be re-added instantly. Only whole-chain deletion confirms.
    model.remove(c, i);
    // Deferred: rebuild() destroys the very block that spawned us.
    scheduleRefresh(false);
}

void ChainRackComponent::scheduleRefresh(bool notifySelection)
{
    // Derive/persist SYNCHRONOUSLY: the model has already been mutated, and
    // the deferred lambda dies silently if the editor is destroyed before the
    // message loop runs it — the RT plan and the <CHAINS> persistence must
    // never depend on the UI surviving one more tick. onChainModelEdited()
    // touches no Component, so it is safe from a block's mouseUp.
    processor.onChainModelEdited();

    // Only the UI part (rebuild destroys the very block whose mouseUp may
    // have called us) is deferred.
    juce::Component::SafePointer<ChainRackComponent> safe(this);
    juce::MessageManager::callAsync([safe, notifySelection]
    {
        if (safe != nullptr)
            safe->refreshAfterModelEdit(notifySelection);
    });
}

//==============================================================================
// Selection
//==============================================================================
ChainBlockId ChainRackComponent::instanceToBlockId(ModuleType type, int chainIdx) const noexcept
{
    switch (type)
    {
        case ModuleType::Pitch:    return ChainBlockId::Pitch;
        case ModuleType::Mask:     return ChainBlockId::Mask;
        case ModuleType::Reverb:   return ChainBlockId::Reverb;
        case ModuleType::Echo:     return ChainBlockId::Echo;
        case ModuleType::Equalizer:return ChainBlockId::Equalizer;
        case ModuleType::Harmonize:return ChainBlockId::Harmonize;
        case ModuleType::Centroid: return ChainBlockId::Centroid;
        case ModuleType::Drive:    return ChainBlockId::Drive;
        case ModuleType::DcBlock:  return ChainBlockId::DcBlock;
        case ModuleType::Gain:     return ChainBlockId::Gain;
        case ModuleType::Sampler:  return ChainBlockId::Sampler;
        case ModuleType::Score:    return ChainBlockId::Score;
        case ModuleType::Timbre:   return ChainBlockId::Timbre;
        case ModuleType::MidiScore:return ChainBlockId::MidiScore;
        case ModuleType::Voice:    return ChainBlockId::Voice;
        case ModuleType::LuxStral: return ChainBlockId::LuxStral;
        case ModuleType::LuxSynth: return ChainBlockId::LuxSynth;
        case ModuleType::LuxWave:  return ChainBlockId::LuxWave;
        case ModuleType::LuxGrain: return ChainBlockId::LuxGrain;
        case ModuleType::VideoScroll: return ChainBlockId::VideoScroll;
        case ModuleType::MidiTap:  return ChainBlockId::MidiTap;
        case ModuleType::Image:    return ChainBlockId::ImageSrc;    // M9 — own pages
        case ModuleType::Video:    return ChainBlockId::VideoSrc;
        case ModuleType::Camera:   return ChainBlockId::CameraSrc;
        case ModuleType::Sp3ctra:
        default:
            return (chainIdx == 0) ? ChainBlockId::Chain1Source
                                   : ChainBlockId::Chain2Source;
    }
}

juce::Uuid ChainRackComponent::firstInstanceId() const
{
    for (const auto& ch : model.chains)
        if (! ch.modules.empty())
            return ch.modules.front().id;
    return {};
}

void ChainRackComponent::selectInstance(const juce::Uuid& id, bool notify)
{
    selectedId = id;
    if (notify)                    // a click / click-equivalent = single selection
        highlightType_.reset();    // (the editor re-applies an ALL-view group)
    applyHighlight();

    if (notify && onBlockSelected)
    {
        int c = -1, i = -1;
        if (auto* m = model.find(id, c, i))
        {
            if (m->type == ModuleType::VideoScroll && onVideoBlockSelected)
                onVideoBlockSelected(m->slot);   // bind the per-instance bank first
            if (m->type == ModuleType::Sampler && onSamplerBlockSelected)
                onSamplerBlockSelected(m->slot);
            if (m->type == ModuleType::LuxStral && onLuxStralBlockSelected)
                onLuxStralBlockSelected(m->slot);   // send slot (0..7, OUT bank)
            onBlockSelected(instanceToBlockId(m->type, c));
        }
    }
}

void ChainRackComponent::setHighlightAllOfType(std::optional<ModuleType> type)
{
    if (highlightType_ == type) return;
    highlightType_ = type;
    applyHighlight();
}

void ChainRackComponent::applyHighlight()
{
    for (auto& blk : blocks)
        blk->setSelected(blk->getUuid() == selectedId
                         || (highlightType_.has_value() && blk->getType() == *highlightType_));
}

void ChainRackComponent::selectInstanceById(const juce::Uuid& id)
{
    int c = -1, i = -1;
    if (model.find(id, c, i) != nullptr)
        selectInstance(id, true);   // fires the same callbacks as a rack click
}

bool ChainRackComponent::selectVideoSlot(int slot)
{
    for (const auto& ch : model.chains)
        for (const auto& m : ch.modules)
            if (m.type == ModuleType::VideoScroll && m.slot == slot)
            {
                selectInstance(m.id, true);
                return true;
            }
    return false;
}

void ChainRackComponent::setSelectedBlock(ChainBlockId id)
{
    // Keep the current instance if it already maps to this block id.
    int c = -1, i = -1;
    if (auto* cur = model.find(selectedId, c, i))
        if (instanceToBlockId(cur->type, c) == id)
        {
            applyHighlight();
            return;
        }

    // Otherwise pick the first instance matching this block id.
    for (int ci = 0; ci < model.numChains(); ++ci)
        for (const auto& m : model.chains[(size_t) ci].modules)
            if (instanceToBlockId(m.type, ci) == id)
            {
                selectInstance(m.id, false);
                return;
            }
}

bool ChainRackComponent::hasBlock(ChainBlockId id) const noexcept
{
    for (int ci = 0; ci < model.numChains(); ++ci)
        for (const auto& m : model.chains[(size_t) ci].modules)
            if (instanceToBlockId(m.type, ci) == id)
                return true;
    return false;
}

ChainBlockId ChainRackComponent::firstBlockId() const noexcept
{
    for (int ci = 0; ci < model.numChains(); ++ci)
        if (! model.chains[(size_t) ci].modules.empty())
            return instanceToBlockId(model.chains[(size_t) ci].modules.front().type, ci);
    return ChainBlockId::None;
}

void ChainRackComponent::setLocked(bool shouldLock)
{
    if (locked == shouldLock)
        return;
    locked = shouldLock;

    rebuild();   // re-creates blocks with the new removable state + tooltips
    repaint();   // chain-level × is painted by the rack itself
}

//==============================================================================
// Drop-target geometry
//==============================================================================
ChainRackComponent::DropTarget
ChainRackComponent::computeDrop(juce::Point<int> localPos, ModuleType type,
                                const juce::Uuid* movingId) const
{
    const int y = localPos.y;

    // "+ CHAIN" row → drop creates a new chain. Validate BEFORE creating it:
    // the chain-count cap and the GLOBAL placement limits (singleton types,
    // slot pools) still apply — otherwise the drop indicator shows green for a
    // drop that will fail and leave a phantom empty chain behind.
    if (addRowRect.contains(localPos))
    {
        const bool ok = model.canAddChain()
                     && model.canInsertIntoNewChain(type, movingId);
        return { -1, 0, ok, true };
    }

    if (bands.empty())
        return { -1, 0, false, false };

    // Pick the last band whose header starts at or before y (the chain the
    // cursor sits in, or the last chain when below everything).
    int chosen = 0;
    for (int k = 0; k < (int) bands.size(); ++k)
        if (y >= bands[(size_t) k].headerY)
            chosen = k;

    const auto& band = bands[(size_t) chosen];
    const int   c    = band.chainIdx;

    int index = 0;
    if (! band.empty)
        for (const auto& s : slots)
            if (s.chainIdx == c && s.bounds.getCentreY() < y)
                ++index;

    const bool valid = model.canInsert(c, type, movingId);
    return { c, index, valid, false };
}

void ChainRackComponent::updateDropFromDetails(const SourceDetails& d)
{
    ModuleType type;
    const juce::Uuid* movingPtr = nullptr;
    juce::Uuid moving;

    if (ModuleDrag::isRackMove(d.description))
    {
        moving = ModuleDrag::uuid(d.description);
        int c = -1, i = -1;
        auto* m = model.find(moving, c, i);
        if (m == nullptr)
        {
            dropTarget = { -1, 0, false, false };
            return;
        }
        type      = m->type;
        movingPtr = &moving;
    }
    else if (! ModuleDrag::moduleType(d.description, type))
    {
        dropTarget = { -1, 0, false, false };
        return;
    }

    dropTarget = computeDrop(d.localPosition, type, movingPtr);
}

//==============================================================================
// juce::DragAndDropTarget
//==============================================================================
bool ChainRackComponent::isInterestedInDragSource(const SourceDetails& d)
{
    return ModuleDrag::isCatalogue(d.description) || ModuleDrag::isRackMove(d.description);
}

void ChainRackComponent::itemDragEnter(const SourceDetails& d)
{
    dragActive = true;
    updateDropFromDetails(d);
    repaint();
}

void ChainRackComponent::itemDragMove(const SourceDetails& d)
{
    updateDropFromDetails(d);
    repaint();
}

void ChainRackComponent::itemDragExit(const SourceDetails&)
{
    dragActive = false;
    repaint();
}

void ChainRackComponent::itemDropped(const SourceDetails& d)
{
    dragActive = false;

    // Resolve dragged type + (optional) moving instance.
    ModuleType type;
    juce::Uuid moving;
    bool isMove = ModuleDrag::isRackMove(d.description);
    int  sc = -1, si = -1;

    if (isMove)
    {
        moving = ModuleDrag::uuid(d.description);
        auto* m = model.find(moving, sc, si);
        if (m == nullptr) { repaint(); return; }
        type = m->type;
    }
    else if (! ModuleDrag::moduleType(d.description, type))
    {
        repaint();
        return;
    }

    const DropTarget dt = computeDrop(d.localPosition, type, isMove ? &moving : nullptr);
    if (! dt.valid)
    {
        repaint();
        return;   // constraint rejected — drop swallowed silently, no change
    }

    if (! isMove)
    {
        const int c = dt.newChain ? model.addChain() : dt.chainIdx;
        if (c < 0) { repaint(); return; }   // chain cap reached
        const int idx = dt.newChain ? 0
                      : juce::jlimit(0, (int) model.chains[(size_t) c].modules.size(), dt.index);
        if (model.insert(c, type, idx))
        {
            const auto& mods = model.chains[(size_t) c].modules;
            const int pos = juce::jlimit(0, (int) mods.size() - 1, idx);
            selectedId = mods[(size_t) pos].id;   // newly added module becomes the selection
        }
        else if (dt.newChain)
        {
            model.removeChain(c);   // rollback — never leave a phantom empty chain
            repaint();
            return;
        }
        scheduleRefresh(true);    // deferred: rebuild() destroys the drag-source block
    }
    else
    {
        if (dt.newChain)
        {
            const int c = model.addChain();
            if (c < 0) { repaint(); return; }   // chain cap reached
            if (! model.moveAcross(sc, si, c, 0))
            {
                model.removeChain(c);   // rollback — never leave a phantom empty chain
                repaint();
                return;
            }
        }
        else if (dt.chainIdx == sc)
        {
            int to = dt.index;
            if (si < to) --to;     // account for the gap left by removing the source
            model.moveWithin(sc, si, to);
        }
        else
        {
            model.moveAcross(sc, si, dt.chainIdx, dt.index);
        }
        scheduleRefresh(false);   // same instance stays selected
    }
}

//==============================================================================
// Layout
//==============================================================================
int ChainRackComponent::preferredHeight() const noexcept
{
    // MUST mirror resized(): per chain a header, then either the padded drop
    // zone (empty) or IN strip + blocks + END strip, then the card gutter.
    int h = kTopPad;
    for (const auto& ch : model.chains)
    {
        h += kHeaderH;
        const int n = (int) ch.modules.size();
        if (n == 0)
            h += kEmptyPad + kEmptyH + kEmptyPad;
        else
            h += kInH + n * kBlockH + (n - 1) * kBlockGap + kEndH;
        h += kChainGap;
    }
    h += kAddRowH + kBottomPad;
    return h;
}

void ChainRackComponent::resized()
{
    slots.clear();
    bands.clear();

    const int bx = kBlockX;
    const int bw = juce::jmax(40, getWidth() - kBlockX - kBlockR);

    int y  = kTopPad;
    int bi = 0;

    for (int c = 0; c < model.numChains(); ++c)
    {
        const auto& mods = model.chains[(size_t) c].modules;

        const int headerY = y;
        y += kHeaderH;

        if (mods.empty())
        {
            const int topY = y + kEmptyPad;
            bands.push_back({ c, headerY, topY, topY + kEmptyH, true,
                              topY + kEmptyH + kEmptyPad });
            y = topY + kEmptyH + kEmptyPad;
        }
        else
        {
            y += kInH;                       // IN marker strip
            const int topY = y;
            for (int i = 0; i < (int) mods.size(); ++i)
            {
                if (bi < (int) blocks.size())
                    blocks[(size_t) bi]->setBounds(bx, y, bw, kBlockH);
                slots.push_back({ c, i, { bx, y, bw, kBlockH } });
                y += kBlockH;
                ++bi;
                if (i + 1 < (int) mods.size())
                    y += kBlockGap;
            }
            const int bottomY = y;
            y += kEndH;                      // END terminator strip
            bands.push_back({ c, headerY, topY, bottomY, false, y });
        }

        y += kChainGap;
    }

    addRowRect = { bx, y, bw, kAddRowH };
}

//==============================================================================
juce::Rectangle<int> ChainRackComponent::bgBadgeRect(const Band& band) const
{
    // Right-aligned in the header band, left of the delete × (which is only
    // shown when >1 chain and unlocked — the badge keeps a stable position
    // regardless, so it never jumps when the × appears).
    constexpr int badgeH = 16;
    return { getWidth() - kPadX - 18 - kBgBadgeW,
             band.headerY + (kHeaderH - badgeH) / 2, kBgBadgeW, badgeH };
}

//==============================================================================
void ChainRackComponent::paint(juce::Graphics& g)
{
    g.fillAll(kColGutter);

    const int w  = getWidth();
    const int bw = juce::jmax(40, w - kBlockX - kBlockR);   // block column width

    // ── One CARD per chain: envelope, header, rail, badge, ×, IN / END ──────
    for (const auto& band : bands)
    {
        const auto      chainCol = chainHeaderColour(band.chainIdx);
        constexpr float kR       = 6.f;
        const juce::Rectangle<float> env((float) kEnvX, (float) band.headerY,
                                         (float) (w - 2 * kEnvX),
                                         (float) (band.envBottomY - band.headerY));

        // Envelope body (+ a faint chain tint), header strip on the top
        // corners, 1 px rule under it, chain-colour rail, border.
        g.setColour(kColCard);
        g.fillRoundedRectangle(env, kR);
        g.setColour(chainCol.withAlpha(0.05f));
        g.fillRoundedRectangle(env, kR);
        {
            juce::Path hdr;
            hdr.addRoundedRectangle(env.getX(), env.getY(), env.getWidth(),
                                    (float) kHeaderH, kR, kR,
                                    true, true, false, false);
            g.setColour(chainCol.withAlpha(0.14f));
            g.fillPath(hdr);
        }
        g.setColour(chainCol.withAlpha(0.30f));
        g.fillRect(env.getX(), env.getY() + (float) kHeaderH - 1.f,
                   env.getWidth(), 1.f);
        g.setColour(chainCol.withAlpha(0.70f));   // rail stops short of the rounded corners
        g.fillRect(env.getX() + 1.f, env.getY() + (float) kHeaderH,
                   (float) kRailW, env.getHeight() - (float) kHeaderH - kR);
        g.setColour(chainCol.withAlpha(0.40f));
        g.drawRoundedRectangle(env, kR, 1.f);

        // Header: numbered pastille + "CHAIN" — the NUMBER carries the
        // identity (the header colour cycles every 3 chains).
        {
            const float d = 16.f;
            const juce::Rectangle<float> dot((float) kPadX + 2.f,
                                             (float) band.headerY
                                                 + ((float) kHeaderH - d) * 0.5f,
                                             d, d);
            g.setColour(chainCol);
            g.fillEllipse(dot);
            g.setColour(kColGutter);
            g.setFont(juce::Font(juce::FontOptions(11.0f)).boldened());
            g.drawText(juce::String(band.chainIdx + 1), dot.toNearestInt(),
                       juce::Justification::centred, false);

            const int labelX = (int) dot.getRight() + 6;
            g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSmall)).boldened());
            g.setColour(chainCol);
            g.drawText("CHAIN", labelX, band.headerY,
                       juce::jmax(10, bgBadgeRect(band).getX() - 4 - labelX), kHeaderH,
                       juce::Justification::centredLeft, true);
        }

        // Chain background badge (schema 4 — the pole is chain-owned):
        // swatch = the background pole, label = the mode.
        {
            const int  bg = processor.chainBackground(band.chainIdx);
            const auto r  = bgBadgeRect(band).toFloat();
            g.setColour(juce::Colour(0xff20242e));
            g.fillRoundedRectangle(r, 3.f);
            g.setColour(juce::Colour(0xff3a4250));
            g.drawRoundedRectangle(r, 3.f, 1.f);

            juce::Rectangle<float> sw(r.getX() + 4.f, r.getCentreY() - 4.f,
                                      8.f, 8.f);
            if (bg == kChainBgAuto)
            {   // half white / half black — "detected from the stream"
                g.setColour(juce::Colours::white);
                g.fillRect(sw.removeFromLeft(4.f));
                g.setColour(juce::Colours::black);
                g.fillRect(sw);
                sw = { r.getX() + 4.f, r.getCentreY() - 4.f, 8.f, 8.f };
            }
            else
            {
                g.setColour(bg == kChainBgWhite ? juce::Colours::white
                                                : juce::Colours::black);
                g.fillRect(sw);
            }
            g.setColour(juce::Colour(0xff6b7280));
            g.drawRect(sw, 1.f);

            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
            g.setColour(juce::Colour(0xff9aa4b4));
            g.drawText(bg == kChainBgAuto  ? "AUTO"
                     : bg == kChainBgBlack ? "BLACK" : "WHITE",
                       (int) r.getX() + 14, (int) r.getY(),
                       (int) r.getWidth() - 16, (int) r.getHeight(),
                       juce::Justification::centredLeft, false);
        }

        if (model.numChains() > 1 && ! locked)   // remove-chain × (hidden when locked)
        {
            const juce::Rectangle<float> x((float) (w - kPadX - 14),
                                           (float) band.headerY
                                               + ((float) kHeaderH - 12.f) * 0.5f,
                                           12.f, 12.f);
            g.setColour(juce::Colour(0xff6b7280));
            const float pad = 2.5f;
            g.drawLine(x.getX() + pad, x.getY() + pad, x.getRight() - pad, x.getBottom() - pad, 1.3f);
            g.drawLine(x.getRight() - pad, x.getY() + pad, x.getX() + pad, x.getBottom() - pad, 1.3f);
        }

        if (band.empty)
        {
            juce::Rectangle<float> z((float) kBlockX, (float) band.topY,
                                     (float) bw,
                                     (float) (band.bottomY - band.topY));
            z = z.reduced(2.f);
            const bool hot = dragActive && ! dropTarget.newChain
                          && dropTarget.chainIdx == band.chainIdx;
            g.setColour(hot ? juce::Colour(0x224ae0a0) : juce::Colour(0x14000000));
            g.fillRoundedRectangle(z, 5.f);
            g.setColour(hot ? juce::Colour(0xff6be0a0) : juce::Colour(0xff3a4250));
            g.drawRoundedRectangle(z, 5.f, 1.f);
            g.setColour(juce::Colour(0xff5a6270));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
            g.drawText("drop module", z.toNearestInt(), juce::Justification::centred, false);
            continue;
        }

        // IN / END markers around the block column — where the flux enters
        // the chain and where it stops.
        const Slot* first = nullptr;
        const Slot* last  = nullptr;
        for (const auto& s : slots)
            if (s.chainIdx == band.chainIdx)
            {
                if (first == nullptr) first = &s;
                last = &s;
            }
        if (first == nullptr)
            continue;

        const float scx = (float) first->bounds.getCentreX();
        g.setFont(juce::FontOptions(10.0f));

        {   // IN — entry port: hollow ring, then a connector into the first block
            const int   stripY = band.headerY + kHeaderH;
            const float ringY  = (float) stripY + 4.5f;
            const float tipY   = (float) first->bounds.getY() + 2.f;
            g.setColour(chainCol.withAlpha(0.80f));
            g.drawEllipse(scx - 3.f, ringY - 3.f, 6.f, 6.f, 1.4f);
            g.drawLine(scx, ringY + 3.f, scx, tipY, 1.4f);
            juce::Path a;
            a.addTriangle(scx - 3.5f, tipY - 4.f, scx + 3.5f, tipY - 4.f, scx, tipY);
            g.fillPath(a);
            g.setColour(kColMarkerTxt);
            g.drawText("IN", kBlockX + 2, stripY, 40, kInH,
                       juce::Justification::centredLeft, false);
        }

        {   // END — terminator under the last block: stub + two shrinking bars
            const float y0 = (float) last->bounds.getBottom() - 2.f;
            const float e  = (float) band.bottomY;
            g.setColour(chainCol.withAlpha(0.80f));
            g.drawLine(scx, y0, scx, e + 6.f, 1.4f);
            g.fillRect(scx - 8.f,  e + 6.f,  16.f, 1.6f);
            g.fillRect(scx - 4.5f, e + 9.5f,  9.f, 1.6f);
            g.setColour(kColMarkerTxt);
            g.drawText("END", kBlockX + 2, band.bottomY, 40, kEndH,
                       juce::Justification::centredLeft, false);
        }
    }

    // ── Flow connectors between consecutive blocks of the same chain ──────────
    g.setColour(kColConnector);
    for (size_t k = 0; k + 1 < slots.size(); ++k)
    {
        if (slots[k].chainIdx != slots[k + 1].chainIdx)
            continue;
        const auto a = slots[k].bounds;
        const auto b = slots[k + 1].bounds;
        const float cx = (float) a.getCentreX();
        const float y0 = (float) a.getBottom() - 2.f;
        const float y1 = (float) b.getY() + 2.f;
        g.setColour(kColConnector);
        g.drawLine(cx, y0, cx, y1, 1.4f);
        juce::Path arrow;
        arrow.addTriangle(cx - 3.5f, y1 - 4.f, cx + 3.5f, y1 - 4.f, cx, y1);
        g.fillPath(arrow);
    }

    // ── Exit arrows: a SEND's flux leaves the chain toward its engine ───────
    // (→ LUXSTRAL / → LUXSYNTH / → LUXWAVE / → LUXGRAIN). Probes (VIDEO SCROLL,
    // MIDI TAP) are pass-through and get none. Drawn after the cards so the
    // arrow pierces the envelope border.
    for (const auto& s : slots)
    {
        const auto& mods = model.chains[(size_t) s.chainIdx].modules;
        if (s.moduleIdx < 0 || s.moduleIdx >= (int) mods.size())
            continue;
        const ModuleType t = mods[(size_t) s.moduleIdx].type;
        if (! ChainModel::isEngineSend(t))
            continue;
        const float x0 = (float) s.bounds.getRight() - 2.f;
        const float x1 = (float) w - 2.f;
        const float cy = (float) s.bounds.getCentreY();
        g.setColour(moduleColour(t).withAlpha(0.85f));
        g.drawLine(x0, cy, x1 - 4.f, cy, 1.6f);
        juce::Path a;
        a.addTriangle(x1 - 5.f, cy - 3.5f, x1 - 5.f, cy + 3.5f, x1, cy);
        g.fillPath(a);
    }

    // ── "+ CHAIN" row (greyed out at the kMaxChains cap) ─────────────────────
    {
        auto r = addRowRect.toFloat().reduced(2.f);
        const bool canAdd = model.canAddChain();
        const bool hot    = canAdd && dragActive && dropTarget.newChain
                                   && dropTarget.valid;
        g.setColour(hot ? juce::Colour(0xff2a3346) : juce::Colour(0xff20242e));
        g.fillRoundedRectangle(r, 4.f);
        g.setColour(hot ? juce::Colour(0xff6be0a0) : juce::Colour(0xff3a4250));
        g.drawRoundedRectangle(r, 4.f, 1.f);
        g.setColour(canAdd ? juce::Colour(0xff9aa4b4) : juce::Colour(0xff4a5058));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontTiny)).boldened());
        g.drawText(canAdd ? "+ CHAIN" : "8 CHAINS MAX",
                   addRowRect, juce::Justification::centred, false);
    }

    // ── Drop indicator (insertion line) ───────────────────────────────────────
    if (dragActive && ! dropTarget.newChain && dropTarget.chainIdx >= 0)
    {
        // Resolve the Y of the insertion boundary inside the target chain.
        int lineY = -1;
        int left  = kBlockX, right = getWidth() - kBlockR;
        std::vector<const Slot*> chainSlots;
        for (const auto& s : slots)
            if (s.chainIdx == dropTarget.chainIdx)
                chainSlots.push_back(&s);

        if (chainSlots.empty())
        {
            for (const auto& band : bands)
                if (band.chainIdx == dropTarget.chainIdx)
                    lineY = (band.topY + band.bottomY) / 2;
        }
        else if (dropTarget.index <= 0)
        {
            lineY = chainSlots.front()->bounds.getY() - kInH / 2;   // mid IN strip
        }
        else
        {
            const int idx = juce::jlimit(0, (int) chainSlots.size() - 1, dropTarget.index - 1);
            lineY = chainSlots[(size_t) idx]->bounds.getBottom() + kBlockGap / 2;
        }

        if (lineY >= 0)
        {
            g.setColour(dropTarget.valid ? juce::Colour(0xff6be0a0) : juce::Colour(0xffe06b6b));
            g.fillRect(left, lineY - 1, right - left, 2);
        }
    }

    // Left + right borders (the left edge is the single divider with the rail)
    g.setColour(juce::Colour(Sp3ctraTheme::kColBorder));
    g.fillRect(0, 0, 1, getHeight());
    g.fillRect(getWidth() - 1, 0, 1, getHeight());
}

//==============================================================================
void ChainRackComponent::mouseUp(const juce::MouseEvent& e)
{
    if (! e.mouseWasClicked())
        return;

    // Chain background badge (left-click): pick the chain's pole. Allowed
    // even when locked — flipping the support (paper ↔ screen) is a
    // performance action, not a structural edit.
    if (! e.mods.isPopupMenu())
    {
        for (const auto& band : bands)
        {
            if (! bgBadgeRect(band).contains(e.getPosition()))
                continue;
            const int chainIdx = band.chainIdx;
            const int current  = processor.chainBackground(chainIdx);
            juce::PopupMenu menu;
            menu.addItem(1 + kChainBgAuto,  "Auto",  true, current == kChainBgAuto);
            menu.addItem(1 + kChainBgBlack, "Black", true, current == kChainBgBlack);
            menu.addItem(1 + kChainBgWhite, "White", true, current == kChainBgWhite);
            const auto click = e.getScreenPosition();
            menu.showMenuAsync(
                juce::PopupMenu::Options()
                    .withTargetComponent(this)
                    .withTargetScreenArea({ click.x, click.y, 1, 1 }),
                [safe = juce::Component::SafePointer<ChainRackComponent>(this),
                 chainIdx](int result)
                {
                    if (result == 0) return;
                    auto* self = safe.getComponent();
                    if (self == nullptr) return;
                    self->processor.setChainBackground(chainIdx, result - 1);
                    self->repaint();
                });
            return;
        }
    }

    // J3 — chain header context menu (right-click): duplicate the chain with
    // its modules AND their settings (dropped where a module can't be
    // duplicated — singletons, exhausted pools).
    if (e.mods.isPopupMenu() && ! locked)
    {
        for (const auto& band : bands)
        {
            const juce::Rectangle<int> header(0, band.headerY,
                                              getWidth(), kHeaderH);
            if (! header.contains(e.getPosition()))
                continue;
            juce::PopupMenu menu;
            menu.addItem(1, "Duplicate chain",
                         model.canAddChain());
            menu.addSeparator();
            menu.addItem(2, "Save chain preset...");
            menu.addItem(3, "Load preset into this chain...");
            menu.addItem(4, "Load preset as new chain...",
                         model.canAddChain());
            const int chainIdx = band.chainIdx;
            // Anchor at the click, not the rack component (which would drop
            // the menu at the component's corner, far from the cursor).
            const auto click = e.getScreenPosition();
            menu.showMenuAsync(
                juce::PopupMenu::Options()
                    .withTargetComponent(this)
                    .withTargetScreenArea({ click.x, click.y, 1, 1 }),
                [this, chainIdx](int result)
                {
                    switch (result)
                    {
                        case 1:
                            // duplicateChain runs the whole edit flow itself
                            // (bindings, inherit, plan, VALUES projection); the
                            // rack refresh is UI-only — refreshAfterModelEdit
                            // rebuilds AND repaints. A bare rebuild()+
                            // onModelChanged() grows the component via setSize,
                            // so JUCE only dirties the new bottom strip: the new
                            // chain's header lands in the un-repainted middle
                            // band (invisible) while the old "+ CHAIN" row keeps
                            // its stale pixels there.
                            if (processor.duplicateChain(chainIdx) >= 0)
                                refreshAfterModelEdit(false);
                            break;
                        case 2: savePresetFlow(chainIdx);  break;
                        case 3: loadPresetFlow(chainIdx);  break;
                        case 4: loadPresetFlow(-1);        break;
                        default: break;
                    }
                });
            return;
        }
    }

    if (addRowRect.contains(e.getPosition()))
    {
        if (model.addChain() >= 0)   // refused at the kMaxChains cap
            mutateAndRefresh(false);
        return;
    }

    if (model.numChains() > 1 && ! locked)   // chain delete disabled while locked
    {
        for (const auto& band : bands)
        {
            const juce::Rectangle<int> x(getWidth() - kPadX - 16, band.headerY, 18, kHeaderH);
            if (x.contains(e.getPosition()))
            {
                // Destructive: the chain and all its module placements go away
                // — confirm first (its modules' settings memory survives).
                const int chainIdx = band.chainIdx;
                const int nModules = (chainIdx >= 0
                                      && chainIdx < (int) model.chains.size())
                    ? (int) model.chains[(size_t) chainIdx].modules.size() : 0;
                const juce::String msg =
                    "Delete CHAIN " + juce::String(chainIdx + 1)
                    + (nModules > 0
                        ? (" and its " + juce::String(nModules) + " module(s)?")
                        : juce::String("?"));
                Sp3ctraDialog::showConfirm(
                    this, "Delete chain", msg, "Delete", "Cancel",
                    [safe = juce::Component::SafePointer<ChainRackComponent>(this),
                     chainIdx](bool ok)
                    {
                        if (! ok) return;
                        auto* self = safe.getComponent();
                        if (self == nullptr) return;
                        if (self->model.numChains() <= 1) return;   // last chain
                        self->model.removeChain(chainIdx);
                        self->mutateAndRefresh(false);
                    });
                return;
            }
        }
    }
}

//==============================================================================
// J4 — .sp3chain preset flows
//==============================================================================
void ChainRackComponent::savePresetFlow(int chainIdx)
{
    if (LicenseGate::blockIfDemo(this, "Save chain preset"))
        return;
    // Presets are reusable across sessions: seed from the last preset folder
    // used (default: Documents/Sp3ctra Chain Presets), not the session dir.
    const auto dir = processor.sessions()->startDirFor(
        PathKeys::chainPreset,
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile("Sp3ctra Chain Presets"));
    dir.createDirectory();
    presetChooser_ = std::make_unique<juce::FileChooser>(
        "Save chain preset",
        dir.getChildFile("Chain " + juce::String(chainIdx + 1) + ".sp3chain"),
        "*.sp3chain");
    presetChooser_->launchAsync(
        juce::FileBrowserComponent::saveMode
            | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, chainIdx](const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File{})
                return;
            if (file.getFileExtension().isEmpty())
                file = file.withFileExtension(".sp3chain");
            processor.sessions()->rememberDirFor(PathKeys::chainPreset, file);
            if (! processor.saveChainPreset(chainIdx, file))
            {
                const juce::String msg = "Could not write\n"
                                       + file.getFullPathName();
                Sp3ctraDialog::showWarning(this, "Chain preset",
                                           msg);
            }
        });
}

void ChainRackComponent::loadPresetFlow(int targetChainIdx)
{
    // Loading INTO an existing non-empty chain replaces its modules — confirm
    // before opening the chooser (loading as a NEW chain destroys nothing).
    if (targetChainIdx >= 0 && targetChainIdx < (int) model.chains.size()
        && ! model.chains[(size_t) targetChainIdx].modules.empty())
    {
        const juce::String msg =
            "Loading a preset replaces the current modules of CHAIN "
            + juce::String(targetChainIdx + 1) + ".";
        Sp3ctraDialog::showConfirm(
            this, "Load chain preset", msg, "Continue", "Cancel",
            [safe = juce::Component::SafePointer<ChainRackComponent>(this),
             targetChainIdx](bool ok)
            {
                if (! ok) return;
                if (auto* self = safe.getComponent())
                    self->loadPresetFlowConfirmed(targetChainIdx);
            });
        return;
    }
    loadPresetFlowConfirmed(targetChainIdx);
}

void ChainRackComponent::loadPresetFlowConfirmed(int targetChainIdx)
{
    const auto dir = processor.sessions()->startDirFor(
        PathKeys::chainPreset,
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile("Sp3ctra Chain Presets"));
    presetChooser_ = std::make_unique<juce::FileChooser>(
        targetChainIdx >= 0 ? "Load preset into this chain"
                            : "Load preset as new chain",
        dir, "*.sp3chain");
    presetChooser_->launchAsync(
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles,
        [this, targetChainIdx](const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (file == juce::File{})
                return;
            processor.sessions()->rememberDirFor(PathKeys::chainPreset, file);
            const auto preset = ChainPresetIO::loadFromFile(file);
            if (! preset.isValid())
            {
                const juce::String msg = file.getFileName()
                                       + " is not a valid .sp3chain preset.";
                Sp3ctraDialog::showWarning(this, "Chain preset",
                                           msg);
                return;
            }
            const auto res = processor.loadChainPreset(preset, targetChainIdx);
            if (res.chainIdx < 0)
            {
                Sp3ctraDialog::showWarning(this, "Chain preset",
                    "Could not load the preset (chain limit reached?).");
                return;
            }
            refreshAfterModelEdit(true);   // rebuild + repaint (see duplicateChain)
            if (! res.skipped.isEmpty())
            {
                const juce::String msg =
                    "Loaded, but some modules could not be placed\n"
                    "(singleton already used elsewhere, or pool exhausted):\n\n"
                    + res.skipped.joinIntoString(", ");
                Sp3ctraDialog::showInfo(this, "Chain preset",
                                        msg);
            }
        });
}

//==============================================================================
// LEDs
//==============================================================================
void ChainRackComponent::timerCallback()
{
    updateLeds();
}

ChainRackComponent::LedState ChainRackComponent::ledFor(BlockComponent& blk, int engineSlot) const
{
    const ModuleType  type = blk.getType();
    const juce::Uuid& uid  = blk.getUuid();

    auto paramOn = [this](const juce::String& id) -> bool
    {
        if (auto* raw = processor.getAPVTS().getRawParameterValue(id))
            return raw->load() >= 0.5f;
        return false;
    };

    // FX inserts: ○ disabled / ● its heartbeat moved since the previous refresh
    // (the module changed the line it was fed) / ◐ enabled but producing
    // nothing — unfed chain, or a stream carrying no material to work on.
    auto fxLed = [&blk](int enabled, juce::uint32 ticks) -> LedState
    {
        const bool moved = blk.ledTickMoved(ticks);   // sample EVERY refresh
        return enabled == 0 ? LedState::Off
             : moved        ? LedState::Active
                            : LedState::Idle;
    };

    switch (type)
    {
        case ModuleType::Sp3ctra:
            return sourceLed;

        // M9 — media sources: ● feeding the chain / ◐ enabled but no media /
        // ○ DISABLED (the LED click toggles the source's ACTIVE param).
        // A loaded source publishes its current line even with the transport
        // stopped (frozen head still drives the chain), so "loaded" — not
        // "playing" — is what makes the flux flow.
        // P5-M3/M4 LED fix: each instance's LED reads ITS OWN engine slot
        // (the default-slot-0 read showed the first instance's state on all).
        case ModuleType::Image:
            if (auto* e = processor.getImageSource(engineSlot >= 0 ? engineSlot : 0))
                return ! e->isEnabled() ? LedState::Off
                     : (e->isLoaded() ? LedState::Active : LedState::Idle);
            return LedState::Off;
        case ModuleType::Video:
            if (auto* e = processor.getVideoSource(engineSlot >= 0 ? engineSlot : 0))
                return ! e->isEnabled() ? LedState::Off
                     : (e->isLoaded() ? LedState::Active : LedState::Idle);
            return LedState::Off;
        case ModuleType::Camera:
            if (auto* e = processor.getCameraSource(engineSlot >= 0 ? engineSlot : 0))
                return e->isOpen() && e->isEnabled() ? LedState::Active
                                                     : LedState::Off;
            return LedState::Off;

        case ModuleType::Pitch:
        {   // per-instance pool slot — bound to the module's UUID (follows moves)
            const LuxPitchState* st = lux_pitch_instance(processor.poolSlotForInstance(uid));
            const bool en = (st->config.enabled != 0);
            const int  v  = (int) st->midi.voice_count;
            return ! en ? LedState::Off : (v > 0 ? LedState::Active : LedState::Idle);
        }
        case ModuleType::Mask:
        {   // per-instance pool slot — bound to the module's UUID (follows moves)
            const LuxMaskState* st = lux_mask_instance(processor.poolSlotForInstance(uid));
            const bool en = (st->config.enabled != 0);
            const int  v  = (int) st->midi.voice_count;
            return ! en ? LedState::Off : (v > 0 ? LedState::Active : LedState::Idle);
        }
        // FX inserts — per-instance pool slot (UUID-bound, like Pitch/Mask).
        // The *_active fields are latches (they only say "state to clear on
        // reset"), so the LED reads the activity heartbeat instead — see fxLed.
        case ModuleType::Reverb:
        {
            const LuxReverbState* st = lux_reverb_instance(processor.poolSlotForInstance(uid));
            return fxLed(st->config.enabled, st->active_ticks);
        }
        case ModuleType::Echo:
        {
            const LuxEchoState* st = lux_echo_instance(processor.poolSlotForInstance(uid));
            return fxLed(st->config.enabled, st->active_ticks);
        }
        case ModuleType::Equalizer:
        {
            const LuxEqState* st = lux_eq_instance(processor.poolSlotForInstance(uid));
            return fxLed(st->config.enabled, st->active_ticks);
        }
        case ModuleType::Harmonize:
        {
            const LuxHarmoState* st = lux_harmo_instance(processor.poolSlotForInstance(uid));
            return fxLed(st->config.enabled, st->active_ticks);
        }
        case ModuleType::Centroid:
        {
            const LuxCentroState* st = lux_centro_instance(processor.poolSlotForInstance(uid));
            return fxLed(st->config.enabled, st->active_ticks);
        }
        case ModuleType::Drive:
        {
            const LuxDriveState* st = lux_drive_instance(processor.poolSlotForInstance(uid));
            return fxLed(st->config.enabled, st->active_ticks);
        }
        case ModuleType::DcBlock:
        {
            const LuxDcBlockState* st = lux_dcblock_instance(processor.poolSlotForInstance(uid));
            return fxLed(st->config.enabled, st->active_ticks);
        }
        case ModuleType::Gain:
        {
            const LuxGainState* st = lux_gain_instance(processor.poolSlotForInstance(uid));
            return fxLed(st->config.enabled, st->active_ticks);
        }

        case ModuleType::Sampler:
            // Per-engine enable (P6): each Sampler instance reads ITS OWN engine
            // slot's enable, so the rack LED shows the right on/off per chain
            // (engine 0 keeps the legacy "luxSamplerEnabled" id).
            return paramOn(engineSlot >= 0 ? fsEngineParam(engineSlot, "Enabled")
                                           : juce::String("luxSamplerEnabled"))
                       ? LedState::Active : LedState::Off;
        case ModuleType::LuxStral:
            // Per-send power (the send's own conditioning bank); the ENGINE
            // enables live on the AUDIO MIX strips (M6).
            return paramOn(engineSlot >= 0 ? lsOutParam(engineSlot, "enabled")
                                           : juce::String("deviceEnabled"))
                       ? LedState::Active : LedState::Off;
        case ModuleType::LuxSynth:
            return paramOn(engineSlot >= 0 ? lxOutParam(engineSlot, "enabled")
                                           : juce::String("luxsynthEnabled"))
                       ? LedState::Active : LedState::Off;
        case ModuleType::LuxWave:
            return paramOn(engineSlot >= 0 ? lwOutParam(engineSlot, "enabled")
                                           : juce::String("luxwaveEnabled"))
                       ? LedState::Active : LedState::Off;
        case ModuleType::LuxGrain:
            return paramOn(engineSlot >= 0 ? lgOutParam(engineSlot, "enabled")
                                           : juce::String("luxgrainEnabled"))
                       ? LedState::Active : LedState::Off;

        // SCORE, TIMBRE, MIDI SCORE and VOICE each own a score-player slot
        // (P5-M4): ○ deactivated / ● feeding its chains / ◐ active but
        // injecting nothing. Like the media sources, the LED follows the
        // REAL flux, not the transport: play, scrub, tail runout — and the
        // P8 parked hold (VOICE with a generated take drones while stopped,
        // so its LED stays full exactly like a loaded IMAGE's).
        case ModuleType::Score:
        case ModuleType::Timbre:
        case ModuleType::MidiScore:
        case ModuleType::Voice:
        {
            const int slot = engineSlot >= 0 ? engineSlot : 0;
            if (! paramOn(scoreActiveParam(slot)))
                return LedState::Off;                 // deactivated
            auto* sc = processor.getScoreChannelForSlot(slot);
            return (sc != nullptr && sc->isScoreFeeding()) ? LedState::Active
                                                           : LedState::Idle;
        }

        case ModuleType::VideoScroll:
        {
            // Per-instance output toggle: LED reflects (and clicking flips) this
            // slot's enable param. Off = the mixer drops this output.
            if (engineSlot < 0)
                return LedState::Off;
            auto* raw = processor.getAPVTS().getRawParameterValue(vsParam(engineSlot, "enabled"));
            return (raw && raw->load() >= 0.5f) ? LedState::Active : LedState::Off;
        }

        case ModuleType::MidiTap:
        {
            // ○ disabled / ● notes were emitted since the last refresh /
            // ◐ armed but silent (unfed chain, or nothing above threshold).
            // Same heartbeat idiom as the FX inserts.
            if (engineSlot < 0)
                return LedState::Off;
            auto* st = midi_tap_instance(engineSlot);
            return fxLed(st ? st->config.enabled : 0,
                         st ? midi_tap_active_ticks(st) : 0u);
        }

        default:
            break;   // types without a rack LED → neutral
    }
    return LedState::Off;
}

void ChainRackComponent::updateLeds()
{
    // Source LED = the transport state first (the dot IS the play/stop
    // switch): ○ stopped / ◐ paused (the frozen frame still feeds the chain)
    // / ● playing with the UDP feed advancing (◐ when live but feed idle).
    {
        bool advancing = false;
        if (auto* core = processor.getSp3ctraCore(); core != nullptr && core->isInitialized())
        {
            if (auto* buffers = core->getAudioImageBuffers();
                buffers != nullptr && buffers->initialized)
            {
                const juce::uint64 lines = (juce::uint64) buffers->lines_received;
                advancing = (lines != lastLinesSeen);
                lastLinesSeen = lines;
            }
        }

        int mode = 0;   // imageFreezeMode: 0 = play / 1 = hold / 2 = stop
        if (auto* raw = processor.getAPVTS().getRawParameterValue("imageFreezeMode"))
            mode = juce::roundToInt(raw->load());

        sourceLed = (mode == 2) ? LedState::Off
                  : (mode == 1) ? LedState::Idle
                  : advancing   ? LedState::Active
                                : LedState::Idle;
    }

    for (auto& blk : blocks)
    {
        int c = -1, i = -1;
        const ModuleInstance* mi = model.find(blk->getUuid(), c, i);
        blk->setLed(ledFor(*blk, mi ? mi->slot : -1));
    }
}
