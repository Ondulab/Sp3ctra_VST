#include "ChainRackComponent.h"
#include "../Sp3ctraCore.h"
#include "../sources/MediaSourceEngines.h"   // M9 — media source LEDs
#include "ChainPresetIO.h"                   // J4 — .sp3chain presets
#include "../Sp3ctraDialog.h"

// C engine state — read-only here (LED monitoring)
extern "C" {
    #include "processing/lux_pitch.h"                 // g_lux_pitch_proc
    #include "processing/lux_mask.h"                  // g_lux_mask_proc
    #include "processing/lux_reverb.h"                // FX pools — LED monitoring
    #include "processing/lux_echo.h"
    #include "processing/lux_eq.h"
    #include "processing/lux_harmo.h"
    #include "audio/buffers/audio_image_buffers.h"    // lines_received counter
}

namespace
{
    // Chain group header colours (cycled per chain index)
    const juce::Colour kColChain1Hdr { 0xffe0b84a }; // amber
    const juce::Colour kColChain2Hdr { 0xff4ae0a0 }; // green
    const juce::Colour kColChain3Hdr { 0xffc0c4cc }; // grey
    const juce::Colour kColConnector { 0xff3a4250 };

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
        case ChainBlockId::Sampler:  return ModuleType::Sampler;
        case ChainBlockId::Score:    return ModuleType::Score;
        case ChainBlockId::Timbre:   return ModuleType::Timbre;
        case ChainBlockId::MidiScore:return ModuleType::MidiScore;
        case ChainBlockId::Voice:    return ModuleType::Voice;
        case ChainBlockId::Sequencer:return ModuleType::Sequencer;
        case ChainBlockId::LuxStral: return ModuleType::LuxStral;
        case ChainBlockId::LuxSynth: return ModuleType::LuxSynth;
        case ChainBlockId::LuxWave:  return ModuleType::LuxWave;
        case ChainBlockId::LuxGrain: return ModuleType::LuxGrain;
        case ChainBlockId::VideoScroll: return ModuleType::VideoScroll;
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

    // Selection halo
    if (selected)
    {
        g.setColour(colour.withAlpha(0.30f));
        g.drawRoundedRectangle(b.expanded(1.2f), 6.5f, 3.0f);
    }

    const juce::Colour bg = selected      ? colour.withAlpha(0.22f)
                          : isMouseOver() ? colour.withAlpha(0.10f)
                          :                 juce::Colour(0xff1a1f2a);
    g.setColour(bg);
    g.fillRoundedRectangle(b, 5.f);
    g.setColour(selected ? colour.withAlpha(0.95f) : colour.withAlpha(0.35f));
    g.drawRoundedRectangle(b, 5.f, selected ? 1.6f : 1.f);

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
    // A tiny keyboard badge leads the label for modules that need a MIDI input.
    auto textArea = b.reduced(9.f, 0.f).withTrimmedRight(40.f);
    if (moduleNeedsMidi(type))
    {
        const float icoW = 12.f, icoH = 11.f;
        const juce::Rectangle<float> iconR(b.getX() + 8.f, b.getCentreY() - icoH * 0.5f,
                                           icoW, icoH);
        ModuleIcons::drawMidiKeyboard(g, iconR, colour.withAlpha(selected ? 0.95f : 0.62f));
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
                || m.type == ModuleType::Equalizer || m.type == ModuleType::Harmonize)
                bp->setEnableParamOverride(insertBankParam(
                    m.type, processor.poolSlotForInstance(m.id), "Enabled"));
            bp->onClick        = [this](juce::Uuid id) { selectInstance(id, true); };
            bp->onToggleEnable = [this, bp]            { toggleEnable(bp->getEnableParam()); };
            bp->onRemove       = [this](juce::Uuid id) { removeInstance(id); };
            // P5-M5 — the score-family LED is the instance's TRANSPORT: click
            // = PLAY/STOP of ITS player slot (uiPlayScore toggles; empty slot
            // = no-op). The sentinel id only arms the LED hit-zone/hover ring
            // — the callback above is replaced, nothing reaches the APVTS.
            if (isScoreFamily(m.type))
            {
                const int scoreSlot = m.slot >= 0 ? m.slot : 0;
                bp->setEnableParamOverride("__scoreTransport");
                bp->onToggleEnable = [this, scoreSlot]
                {
                    if (auto* sc = processor.getScoreChannelForSlot(scoreSlot))
                        sc->uiPlayScore();
                };
            }
            bp->setRemovable(! locked);
            const bool hasLed = bp->getEnableParam().isNotEmpty();
            const bool isTransportLed = isScoreFamily(m.type);
            bp->setTooltip(isTransportLed
                ? (locked ? "Click the LED to play/stop - drag to reorder"
                          : "Click the LED to play/stop - drag to reorder - x to remove")
                : hasLed
                ? (locked ? "Click the LED to enable/disable - drag to reorder"
                          : "Click the LED to enable/disable - drag to reorder - x to remove")
                : (locked ? "Drag to reorder" : "Drag to reorder - x to remove"));
            bp->setSelected(m.id == selectedId);
            addAndMakeVisible(bp);
            blocks.push_back(std::move(blk));
        }
    }

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
    for (auto& blk : blocks)
        blk->setSelected(blk->getUuid() == selectedId);

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
    if (model.find(id, c, i) == nullptr)
        return;
    model.remove(c, i);
    // Deferred: rebuild() destroys the very block whose mouseUp called us.
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
        case ModuleType::Sampler:  return ChainBlockId::Sampler;
        case ModuleType::Score:    return ChainBlockId::Score;
        case ModuleType::Timbre:   return ChainBlockId::Timbre;
        case ModuleType::MidiScore:return ChainBlockId::MidiScore;
        case ModuleType::Voice:    return ChainBlockId::Voice;
        case ModuleType::Sequencer:return ChainBlockId::Sequencer;
        case ModuleType::LuxStral: return ChainBlockId::LuxStral;
        case ModuleType::LuxSynth: return ChainBlockId::LuxSynth;
        case ModuleType::LuxWave:  return ChainBlockId::LuxWave;
        case ModuleType::LuxGrain: return ChainBlockId::LuxGrain;
        case ModuleType::VideoScroll: return ChainBlockId::VideoScroll;
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
    for (auto& blk : blocks)
        blk->setSelected(blk->getUuid() == id);

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

void ChainRackComponent::selectInstanceById(const juce::Uuid& id)
{
    int c = -1, i = -1;
    if (model.find(id, c, i) != nullptr)
        selectInstance(id, true);   // fires the same callbacks as a rack click
}

void ChainRackComponent::setSelectedBlock(ChainBlockId id)
{
    // Keep the current instance if it already maps to this block id.
    int c = -1, i = -1;
    if (auto* cur = model.find(selectedId, c, i))
        if (instanceToBlockId(cur->type, c) == id)
        {
            for (auto& blk : blocks)
                blk->setSelected(blk->getUuid() == selectedId);
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
    int h = kTopPad;
    for (const auto& ch : model.chains)
    {
        h += kHeaderH + 2;
        const int n = (int) ch.modules.size();
        if (n == 0)
            h += kEmptyH;
        else
            h += n * kBlockH + (n - 1) * kBlockGap;
        h += kChainGap;
    }
    h += kAddRowH + kBottomPad;
    return h;
}

void ChainRackComponent::resized()
{
    slots.clear();
    bands.clear();

    const int bx = kPadX;
    const int bw = juce::jmax(40, getWidth() - 2 * kPadX);

    int y  = kTopPad;
    int bi = 0;

    for (int c = 0; c < model.numChains(); ++c)
    {
        const auto& mods = model.chains[(size_t) c].modules;

        const int headerY = y;
        y += kHeaderH + 2;
        const int topY = y;

        if (mods.empty())
        {
            bands.push_back({ c, headerY, topY, topY + kEmptyH, true });
            y += kEmptyH;
        }
        else
        {
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
            bands.push_back({ c, headerY, topY, y, false });
        }

        y += kChainGap;
    }

    addRowRect = { bx, y, bw, kAddRowH };
}

//==============================================================================
void ChainRackComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff181820));

    // ── Per-chain headers, × buttons, empty drop zones ───────────────────────
    for (const auto& band : bands)
    {
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSmall)).boldened());
        g.setColour(chainHeaderColour(band.chainIdx));
        g.drawText("CHAIN " + juce::String(band.chainIdx + 1),
                   kPadX + 2, band.headerY, getWidth() - 2 * kPadX, kHeaderH,
                   juce::Justification::centredLeft, true);

        if (model.numChains() > 1 && ! locked)   // remove-chain × (hidden when locked)
        {
            const juce::Rectangle<float> x((float) (getWidth() - kPadX - 14),
                                           (float) band.headerY + 2.f, 12.f, 12.f);
            g.setColour(juce::Colour(0xff6b7280));
            const float pad = 2.5f;
            g.drawLine(x.getX() + pad, x.getY() + pad, x.getRight() - pad, x.getBottom() - pad, 1.3f);
            g.drawLine(x.getRight() - pad, x.getY() + pad, x.getX() + pad, x.getBottom() - pad, 1.3f);
        }

        if (band.empty)
        {
            juce::Rectangle<float> z((float) kPadX, (float) band.topY,
                                     (float) juce::jmax(40, getWidth() - 2 * kPadX),
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
        int left  = kPadX, right = getWidth() - kPadX;
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
            lineY = chainSlots.front()->bounds.getY() - 2;
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
                model.removeChain(band.chainIdx);
                mutateAndRefresh(false);
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
    const auto dir = juce::File::getSpecialLocation(
                         juce::File::userDocumentsDirectory)
                         .getChildFile("Sp3ctra Chain Presets");
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
            if (! processor.saveChainPreset(chainIdx, file))
            {
                const juce::String msg = "Could not write\n"
                                       + file.getFullPathName();
                Sp3ctraDialog::showWarning(this, "Chain preset",
                                           msg.toRawUTF8());
            }
        });
}

void ChainRackComponent::loadPresetFlow(int targetChainIdx)
{
    const auto dir = juce::File::getSpecialLocation(
                         juce::File::userDocumentsDirectory)
                         .getChildFile("Sp3ctra Chain Presets");
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
            const auto preset = ChainPresetIO::loadFromFile(file);
            if (! preset.isValid())
            {
                const juce::String msg = file.getFileName()
                                       + " is not a valid .sp3chain preset.";
                Sp3ctraDialog::showWarning(this, "Chain preset",
                                           msg.toRawUTF8());
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
                                        msg.toRawUTF8());
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

ChainRackComponent::LedState ChainRackComponent::ledFor(ModuleType type, const juce::Uuid& uid, int engineSlot) const
{
    auto paramOn = [this](const juce::String& id) -> bool
    {
        if (auto* raw = processor.getAPVTS().getRawParameterValue(id))
            return raw->load() >= 0.5f;
        return false;
    };

    switch (type)
    {
        case ModuleType::Sp3ctra:
            return sourceLed;

        // M9 — media sources: ● transport running / ◐ media ready / ○ nothing
        // or DISABLED (the LED click toggles the source's ACTIVE param).
        // P5-M3/M4 LED fix: each instance's LED reads ITS OWN engine slot
        // (the default-slot-0 read showed the first instance's state on all).
        case ModuleType::Image:
            if (auto* e = processor.getImageSource(engineSlot >= 0 ? engineSlot : 0))
                return ! e->isLoaded() || ! e->isEnabled() ? LedState::Off
                     : (e->isPlaying() ? LedState::Active : LedState::Idle);
            return LedState::Off;
        case ModuleType::Video:
            if (auto* e = processor.getVideoSource(engineSlot >= 0 ? engineSlot : 0))
                return ! e->isLoaded() || ! e->isEnabled() ? LedState::Off
                     : (e->isPlaying() ? LedState::Active : LedState::Idle);
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
        // FX inserts — per-instance pool slot (UUID-bound, like Pitch/Mask):
        // ● processing a stream / ◐ enabled but idle / ○ disabled.
        case ModuleType::Reverb:
        {
            const LuxReverbState* st = lux_reverb_instance(processor.poolSlotForInstance(uid));
            return st->config.enabled == 0 ? LedState::Off
                 : (st->tail_active != 0   ? LedState::Active : LedState::Idle);
        }
        case ModuleType::Echo:
        {
            const LuxEchoState* st = lux_echo_instance(processor.poolSlotForInstance(uid));
            return st->config.enabled == 0 ? LedState::Off
                 : (st->ring_active != 0   ? LedState::Active : LedState::Idle);
        }
        case ModuleType::Equalizer:
        {
            const LuxEqState* st = lux_eq_instance(processor.poolSlotForInstance(uid));
            return st->config.enabled == 0 ? LedState::Off
                 : (st->eq_active != 0     ? LedState::Active : LedState::Idle);
        }
        case ModuleType::Harmonize:
        {
            const LuxHarmoState* st = lux_harmo_instance(processor.poolSlotForInstance(uid));
            return st->config.enabled == 0 ? LedState::Off
                 : (st->harmo_active != 0  ? LedState::Active : LedState::Idle);
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
        // (P5-M4): the block LED reflects THIS instance's transport —
        // ● playing / ◐ content loaded / ○ empty.
        case ModuleType::Score:
        case ModuleType::Timbre:
        case ModuleType::MidiScore:
        case ModuleType::Voice:
            if (auto* sc = processor.getScoreChannelForSlot(
                    engineSlot >= 0 ? engineSlot : 0))
                return sc->isScorePlaying()  ? LedState::Active
                     : sc->scoreHasContent() ? LedState::Idle
                     :                         LedState::Off;
            return LedState::Off;

        case ModuleType::VideoScroll:
        {
            // Per-instance output toggle: LED reflects (and clicking flips) this
            // slot's enable param. Off = the mixer drops this output.
            if (engineSlot < 0)
                return LedState::Off;
            auto* raw = processor.getAPVTS().getRawParameterValue(vsParam(engineSlot, "enabled"));
            return (raw && raw->load() >= 0.5f) ? LedState::Active : LedState::Off;
        }

        default:
            break;   // Sequencer (owned elsewhere) etc. → neutral
    }
    return LedState::Off;
}

void ChainRackComponent::updateLeds()
{
    // Source activity: LED active while the UDP feed advances.
    {
        LedState src = LedState::Off;
        if (auto* core = processor.getSp3ctraCore(); core != nullptr && core->isInitialized())
        {
            if (auto* buffers = core->getAudioImageBuffers();
                buffers != nullptr && buffers->initialized)
            {
                const juce::uint64 lines = (juce::uint64) buffers->lines_received;
                src = (lines != lastLinesSeen) ? LedState::Active : LedState::Idle;
                lastLinesSeen = lines;
            }
        }
        sourceLed = src;
    }

    for (auto& blk : blocks)
    {
        int c = -1, i = -1;
        const ModuleInstance* mi = model.find(blk->getUuid(), c, i);
        blk->setLed(ledFor(blk->getType(), blk->getUuid(), mi ? mi->slot : -1));
    }
}
