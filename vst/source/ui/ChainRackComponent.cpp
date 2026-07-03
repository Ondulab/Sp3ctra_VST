#include "ChainRackComponent.h"
#include "../Sp3ctraCore.h"
#include "../sources/MediaSourceEngines.h"   // M9 — media source LEDs

// C engine state — read-only here (LED monitoring)
extern "C" {
    #include "processing/lux_pitch.h"                 // g_lux_pitch_proc
    #include "processing/lux_mask.h"                  // g_lux_mask_proc
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
        case ChainBlockId::Sampler:  return ModuleType::Sampler;
        case ChainBlockId::Score:    return ModuleType::Score;
        case ChainBlockId::Sequencer:return ModuleType::Sequencer;
        case ChainBlockId::LuxStral: return ModuleType::LuxStral;
        case ChainBlockId::LuxSynth: return ModuleType::LuxSynth;
        case ChainBlockId::LuxWave:  return ModuleType::LuxWave;
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
    return moduleColour(chainBlockToModuleType(id));
}

juce::String ChainRackComponent::enableParamId(ChainBlockId id) noexcept
{
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
    g.setColour(selected ? juce::Colours::white : colour.brighter(0.3f));
    g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
    g.drawText(name,
               b.reduced(9.f, 0.f).withTrimmedRight(40.f).toNearestInt(),
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

    // M8 — when both LuxStral engines (A/B) are placed, badge each block with its
    // engine letter so they can be told apart. A lone engine stays plain "LUXSTRAL".
    int luxstralCount = 0;
    for (const auto& ch : model.chains)
        for (const auto& m : ch.modules)
            if (m.type == ModuleType::LuxStral) ++luxstralCount;

    for (int c = 0; c < model.numChains(); ++c)
    {
        for (auto& m : model.chains[(size_t) c].modules)
        {
            auto blk = std::make_unique<BlockComponent>(m.type, m.id);
            auto* bp = blk.get();
            if (m.type == ModuleType::LuxStral && luxstralCount >= 2 && m.slot >= 0)
                bp->setEngineSuffix(m.slot == 1 ? "B" : "A");
            // Engine B's LED toggles its OWN enable param (independent of A).
            if (m.type == ModuleType::LuxStral && m.slot == 1)
                bp->setEnableParamOverride("luxstralBEnabled");
            // Each VideoScroll output is per-instance: its LED toggles the slot's
            // own enable param, so the mixer can drop just this output.
            if (m.type == ModuleType::VideoScroll && m.slot >= 0)
                bp->setEnableParamOverride(vsParam(m.slot, "enabled"));
            bp->onClick        = [this](juce::Uuid id) { selectInstance(id, true); };
            bp->onToggleEnable = [this, bp]            { toggleEnable(bp->getEnableParam()); };
            bp->onRemove       = [this](juce::Uuid id) { removeInstance(id); };
            bp->setRemovable(! locked);
            const bool hasLed = bp->getEnableParam().isNotEmpty();
            bp->setTooltip(hasLed
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
                onSamplerBlockSelected(m->slot);   // engine A (0) / B (1)
            if (m->type == ModuleType::LuxStral && onLuxStralBlockSelected)
                onLuxStralBlockSelected(m->slot == 1 ? 1 : 0);   // engine A (0) / B (1)
            onBlockSelected(instanceToBlockId(m->type, sc));
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
    juce::Component::SafePointer<ChainRackComponent> safe(this);
    juce::MessageManager::callAsync([safe, notifySelection]
    {
        if (safe != nullptr)
            safe->mutateAndRefresh(notifySelection);
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
        case ModuleType::Sampler:  return ChainBlockId::Sampler;
        case ModuleType::Score:    return ChainBlockId::Score;
        case ModuleType::Sequencer:return ChainBlockId::Sequencer;
        case ModuleType::LuxStral: return ChainBlockId::LuxStral;
        case ModuleType::LuxSynth: return ChainBlockId::LuxSynth;
        case ModuleType::LuxWave:  return ChainBlockId::LuxWave;
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
                onSamplerBlockSelected(m->slot);   // engine A (0) / B (1)
            if (m->type == ModuleType::LuxStral && onLuxStralBlockSelected)
                onLuxStralBlockSelected(m->slot == 1 ? 1 : 0);   // engine A (0) / B (1)
            onBlockSelected(instanceToBlockId(m->type, c));
        }
    }
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
// LEDs
//==============================================================================
void ChainRackComponent::timerCallback()
{
    updateLeds();
}

ChainRackComponent::LedState ChainRackComponent::ledFor(ModuleType type, int chainIdx, int engineSlot) const
{
    auto paramOn = [this](const char* id) -> bool
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
        case ModuleType::Image:
            if (auto* e = processor.getImageSource())
                return ! e->isLoaded() ? LedState::Off
                     : (e->isPlaying() ? LedState::Active : LedState::Idle);
            return LedState::Off;
        case ModuleType::Video:
            if (auto* e = processor.getVideoSource())
                return ! e->isLoaded() ? LedState::Off
                     : (e->isPlaying() ? LedState::Active : LedState::Idle);
            return LedState::Off;
        case ModuleType::Camera:
            if (auto* e = processor.getCameraSource())
                return e->isOpen() ? LedState::Active : LedState::Off;
            return LedState::Off;

        case ModuleType::Pitch:
        {   // per-chain instance — pool slot is UUID-bound (stable across edits)
            const LuxPitchState* st = lux_pitch_instance(processor.poolSlotForChain(chainIdx));
            const bool en = (st->config.enabled != 0);
            const int  v  = (int) st->midi.voice_count;
            return ! en ? LedState::Off : (v > 0 ? LedState::Active : LedState::Idle);
        }
        case ModuleType::Mask:
        {   // per-chain instance — pool slot is UUID-bound (stable across edits)
            const LuxMaskState* st = lux_mask_instance(processor.poolSlotForChain(chainIdx));
            const bool en = (st->config.enabled != 0);
            const int  v  = (int) st->midi.voice_count;
            return ! en ? LedState::Off : (v > 0 ? LedState::Active : LedState::Idle);
        }
        case ModuleType::Sampler:
            return paramOn("luxSamplerEnabled") ? LedState::Active : LedState::Off;
        case ModuleType::LuxStral:
            // Engine A → deviceEnabled, engine B → luxstralBEnabled (independent).
            return paramOn(engineSlot == 1 ? "luxstralBEnabled" : "deviceEnabled")
                       ? LedState::Active : LedState::Off;
        case ModuleType::LuxSynth:
            return paramOn("luxsynthEnabled") ? LedState::Active : LedState::Off;
        case ModuleType::LuxWave:
            return paramOn("luxwaveEnabled") ? LedState::Active : LedState::Off;

        case ModuleType::Score:
            if (auto* fs = processor.getLuxSampler())
                return fs->isScorePlaying()  ? LedState::Active
                     : fs->scoreHasContent() ? LedState::Idle
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
        blk->setLed(ledFor(blk->getType(), c < 0 ? 0 : c, mi ? mi->slot : -1));
    }
}
