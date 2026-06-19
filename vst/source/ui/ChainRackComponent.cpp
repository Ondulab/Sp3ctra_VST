#include "ChainRackComponent.h"
#include "../Sp3ctraCore.h"

// C engine state — read-only here (LED monitoring)
extern "C" {
    #include "processing/lux_pitch.h"                 // g_lux_pitch_proc
    #include "processing/lux_mask.h"                  // g_lux_mask_proc
    #include "audio/buffers/audio_image_buffers.h"    // lines_received counter
}

namespace
{
    // ── Block identity colours ────────────────────────────────────────────────
    const juce::Colour kColSource   { 0xff68788f };  // neutral grey-blue (matches RAW node)
    const juce::Colour kColPitch    { 0xffe06bb8 };  // pink   (LuxPitch identity)
    const juce::Colour kColMask     { 0xff6be0d0 };  // teal   (LuxMask identity)
    const juce::Colour kColSampler  { 0xffe09040 };  // orange
    const juce::Colour kColLuxStral { 0xff4fa3e0 };  // blue   (Sp3ctraTheme accent Lux)
    const juce::Colour kColLuxSynth { 0xffb07af0 };  // purple
    const juce::Colour kColLuxWave  { 0xff8fd05a };  // yellow-green

    const juce::Colour kColChain1Hdr { 0xffe0b84a }; // amber  ("A - Modulated" identity)
    const juce::Colour kColChain2Hdr { 0xff4ae0a0 }; // green  ("B - Live" identity)
    const juce::Colour kColConnector { 0xff3a4250 };

    juce::String noteName(const char* s) { return juce::String::fromUTF8(s); }
}

//==============================================================================
// Block identity colours — shared accessor
//==============================================================================
juce::Colour ChainRackComponent::blockColour(ChainBlockId id) noexcept
{
    switch (id)
    {
        case ChainBlockId::Pitch:    return kColPitch;
        case ChainBlockId::Mask:     return kColMask;
        case ChainBlockId::Sampler:  return kColSampler;
        case ChainBlockId::LuxStral: return kColLuxStral;
        case ChainBlockId::LuxSynth: return kColLuxSynth;
        case ChainBlockId::LuxWave:  return kColLuxWave;
        case ChainBlockId::Chain1Source:
        case ChainBlockId::Chain2Source:
        default:                     return kColSource;
    }
}

//==============================================================================
// BlockComponent
//==============================================================================
void ChainRackComponent::BlockComponent::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced(2.f);

    // Selection halo (soft outer glow)
    if (selected)
    {
        g.setColour(colour.withAlpha(0.30f));
        g.drawRoundedRectangle(b.expanded(1.2f), 6.5f, 3.0f);
    }

    // Background — same visual language as PipelineNodeComponent
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
        const juce::Rectangle<float> dot(cx - r, cy - r, 2*r, 2*r);

        switch (led)
        {
            case LedState::Active:
                g.setColour(colour.withAlpha(0.30f));            // glow
                g.fillEllipse(dot.expanded(2.5f));
                g.setColour(colour.brighter(0.25f));
                g.fillEllipse(dot);
                break;

            case LedState::Idle:                                  // ◐ — half-lit
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

    // ── Name ──────────────────────────────────────────────────────────────────
    g.setColour(selected ? juce::Colours::white : colour.brighter(0.3f));
    g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
    g.drawText(name,
               b.reduced(9.f, 0.f).withTrimmedRight(16.f).toNearestInt(),
               juce::Justification::centredLeft, true);
}

//==============================================================================
// SwapOrderButton — two opposed vertical arrows (⇅) drawn with paths
//==============================================================================
void ChainRackComponent::SwapOrderButton::paintButton(juce::Graphics& g,
                                                      bool isMouseOver,
                                                      bool isButtonDown)
{
    const auto b = getLocalBounds().toFloat().reduced(1.f);

    const juce::Colour bg(0xff222836);
    g.setColour(isButtonDown ? bg.brighter(0.30f)
              : isMouseOver  ? bg.brighter(0.12f)
              :                bg);
    g.fillRoundedRectangle(b, 3.f);
    g.setColour(juce::Colour(0xff3a4250));
    g.drawRoundedRectangle(b, 3.f, 1.f);

    const juce::Colour fg = isMouseOver ? juce::Colour(0xffe8eef8)
                                        : juce::Colour(0xffaab4c8);
    g.setColour(fg);

    const float top  = b.getY() + 3.5f;
    const float bot  = b.getBottom() - 3.5f;
    const float lx   = b.getCentreX() - 3.5f;   // up arrow column
    const float rx   = b.getCentreX() + 3.5f;   // down arrow column
    const float head = 3.0f;

    juce::Path p;
    // Up arrow (left column)
    p.startNewSubPath(lx, bot);  p.lineTo(lx, top);
    p.startNewSubPath(lx - head, top + head); p.lineTo(lx, top); p.lineTo(lx + head, top + head);
    // Down arrow (right column)
    p.startNewSubPath(rx, top);  p.lineTo(rx, bot);
    p.startNewSubPath(rx - head, bot - head); p.lineTo(rx, bot); p.lineTo(rx + head, bot - head);
    g.strokePath(p, juce::PathStrokeType(1.4f));
}

//==============================================================================
// ChainRackComponent
//==============================================================================
ChainRackComponent::ChainRackComponent(Sp3ctraAudioProcessor& p)
    : processor(p),
      srcABlock  (ChainBlockId::Chain1Source, "SOURCE CIS", kColSource),
      pitchBlock (ChainBlockId::Pitch,        "PITCH",      kColPitch),
      maskBlock  (ChainBlockId::Mask,         "MASK",       kColMask),
      samplerBlock(ChainBlockId::Sampler,     "SAMPLER",    kColSampler),
      stralBlock (ChainBlockId::LuxStral,     noteName("\xE2\x99\xAA LUXSTRAL"), kColLuxStral),
      srcBBlock  (ChainBlockId::Chain2Source, "SOURCE CIS", kColSource),
      synthBlock (ChainBlockId::LuxSynth,     noteName("\xE2\x99\xAA LUXSYNTH"), kColLuxSynth),
      waveBlock  (ChainBlockId::LuxWave,      noteName("\xE2\x99\xAA LUXWAVE"),  kColLuxWave)
{
    for (auto* blk : { &srcABlock, &pitchBlock, &maskBlock, &samplerBlock, &stralBlock,
                       &srcBBlock, &synthBlock, &waveBlock })
    {
        addAndMakeVisible(blk);
        blk->onClick = [this](ChainBlockId id)
        {
            setSelectedBlock(id);
            if (onBlockSelected) onBlockSelected(id);
        };
    }

    swapBtn.setTooltip("Swap insert order (Pitch <-> Mask)");
    swapBtn.onClick = [this] { toggleInsertOrder(); };
    addAndMakeVisible(swapBtn);

    // Reflect external order changes (host automation, settings…)
    processor.getAPVTS().addParameterListener("chainInsertOrder", this);

    updateLeds();
    startTimerHz(10);          // LED refresh
}

ChainRackComponent::~ChainRackComponent()
{
    processor.getAPVTS().removeParameterListener("chainInsertOrder", this);
    stopTimer();
}

//==============================================================================
void ChainRackComponent::setSelectedBlock(ChainBlockId id)
{
    for (auto* blk : { &srcABlock, &pitchBlock, &maskBlock, &samplerBlock, &stralBlock,
                       &srcBBlock, &synthBlock, &waveBlock })
        blk->setSelected(false);

    switch (id)
    {
        case ChainBlockId::Chain1Source: srcABlock  .setSelected(true); break;
        case ChainBlockId::Pitch:        pitchBlock .setSelected(true); break;
        case ChainBlockId::Mask:         maskBlock  .setSelected(true); break;
        case ChainBlockId::Sampler:      samplerBlock.setSelected(true); break;
        case ChainBlockId::LuxStral:     stralBlock .setSelected(true); break;
        case ChainBlockId::Chain2Source: srcBBlock  .setSelected(true); break;
        case ChainBlockId::LuxSynth:     synthBlock .setSelected(true); break;
        case ChainBlockId::LuxWave:      waveBlock  .setSelected(true); break;
    }
}

//==============================================================================
bool ChainRackComponent::isMaskFirst() const
{
    if (auto* raw = processor.getAPVTS().getRawParameterValue("chainInsertOrder"))
        return raw->load() >= 0.5f;     // 0 = Pitch > Mask, 1 = Mask > Pitch
    return false;
}

void ChainRackComponent::toggleInsertOrder()
{
    if (auto* param = processor.getAPVTS().getParameter("chainInsertOrder"))
    {
        const float newNorm = (param->getValue() < 0.5f) ? 1.0f : 0.0f;
        param->beginChangeGesture();
        param->setValueNotifyingHost(newNorm);
        param->endChangeGesture();
    }
    // parameterChanged() relayouts asynchronously, but do it now for snappy UI
    resized();
    repaint();
}

void ChainRackComponent::parameterChanged(const juce::String& paramID, float)
{
    if (paramID != "chainInsertOrder")
        return;

    juce::Component::SafePointer<ChainRackComponent> safe(this);
    juce::MessageManager::callAsync([safe]
    {
        if (safe != nullptr)
        {
            safe->resized();
            safe->repaint();
        }
    });
}

//==============================================================================
std::vector<ChainRackComponent::BlockComponent*> ChainRackComponent::chain1Order()
{
    if (isMaskFirst())
        return { &srcABlock, &maskBlock, &pitchBlock, &samplerBlock, &stralBlock };
    return     { &srcABlock, &pitchBlock, &maskBlock, &samplerBlock, &stralBlock };
}

std::vector<ChainRackComponent::BlockComponent*> ChainRackComponent::chain2Order()
{
    return { &srcBBlock, &synthBlock, &waveBlock };
}

//==============================================================================
int ChainRackComponent::preferredHeight() const noexcept
{
    const int chain1H = (kHeaderH + 2)
                      + 5 * kBlockH
                      + 3 * kBlockGap + kSwapGap;   // 4 gaps: one is the swap gap
    const int chain2H = (kHeaderH + 2)
                      + 3 * kBlockH
                      + 2 * kBlockGap;
    return kTopPad + chain1H + kChainGap + chain2H + kBottomPad;
}

void ChainRackComponent::resized()
{
    const int w  = getWidth();
    const int bx = kPadX;
    const int bw = juce::jmax(40, w - 2 * kPadX);

    int y = kTopPad;

    // ── CHAIN 1 ───────────────────────────────────────────────────────────────
    header1Y = y;
    y += kHeaderH + 2;

    const auto c1 = chain1Order();
    for (size_t i = 0; i < c1.size(); ++i)
    {
        c1[i]->setBounds(bx, y, bw, kBlockH);
        y += kBlockH;
        if (i + 1 < c1.size())
        {
            // The gap between the two inserts (indices 1 and 2) hosts the ⇅ button
            const bool isSwapGap = (i == 1);
            const int  gap       = isSwapGap ? kSwapGap : kBlockGap;
            if (isSwapGap)
                swapBtn.setBounds(getWidth() / 2 + 12, y + (gap - 18) / 2, 24, 18);
            y += gap;
        }
    }

    y += kChainGap;

    // ── CHAIN 2 ───────────────────────────────────────────────────────────────
    header2Y = y;
    y += kHeaderH + 2;

    const auto c2 = chain2Order();
    for (size_t i = 0; i < c2.size(); ++i)
    {
        c2[i]->setBounds(bx, y, bw, kBlockH);
        y += kBlockH;
        if (i + 1 < c2.size())
            y += kBlockGap;
    }
}

//==============================================================================
void ChainRackComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff181820));

    // Group headers
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSmall)).boldened());
    g.setColour(kColChain1Hdr);
    g.drawText("CHAIN 1 - MODULATED", kPadX + 2, header1Y, getWidth() - 2 * kPadX, kHeaderH,
               juce::Justification::centredLeft, true);
    g.setColour(kColChain2Hdr);
    g.drawText("CHAIN 2 - LIVE", kPadX + 2, header2Y, getWidth() - 2 * kPadX, kHeaderH,
               juce::Justification::centredLeft, true);

    // Flow connectors (top → bottom) between consecutive blocks
    auto drawConnectors = [&g](const std::vector<BlockComponent*>& order)
    {
        g.setColour(kColConnector);
        for (size_t i = 0; i + 1 < order.size(); ++i)
        {
            const auto a = order[i]->getBounds();
            const auto b = order[i + 1]->getBounds();
            const float cx = (float)a.getCentreX();
            const float y0 = (float)a.getBottom() - 2.f;
            const float y1 = (float)b.getY() + 2.f;
            g.drawLine(cx, y0, cx, y1, 1.4f);
            // arrowhead
            juce::Path arrow;
            arrow.addTriangle(cx - 3.5f, y1 - 4.f, cx + 3.5f, y1 - 4.f, cx, y1);
            g.fillPath(arrow);
        }
    };
    drawConnectors(chain1Order());
    drawConnectors(chain2Order());

    // Right border (visual separation from the splitter)
    g.setColour(juce::Colour(Sp3ctraTheme::kColBorder));
    g.fillRect(getWidth() - 1, 0, 1, getHeight());
}

//==============================================================================
void ChainRackComponent::timerCallback()
{
    updateLeds();
}

void ChainRackComponent::updateLeds()
{
    auto& apvts = processor.getAPVTS();
    auto paramOn = [&apvts](const char* id) -> bool
    {
        if (auto* raw = apvts.getRawParameterValue(id))
            return raw->load() >= 0.5f;
        return false;
    };

    // ── Sources: LED active while the UDP feed advances ──────────────────────
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
        srcABlock.setLed(src);
        srcBBlock.setLed(src);
    }

    // ── Pitch / Mask inserts: enabled + at least one MIDI voice → active ─────
    {
        const bool pitchEnabled = (g_lux_pitch_proc.config.enabled != 0);
        const int  pitchVoices  = (int) g_lux_pitch_proc.midi.voice_count;
        pitchBlock.setLed(!pitchEnabled    ? LedState::Off
                         : pitchVoices > 0 ? LedState::Active
                         :                   LedState::Idle);

        const bool maskEnabled = (g_lux_mask_proc.config.enabled != 0);
        const int  maskVoices  = (int) g_lux_mask_proc.midi.voice_count;
        maskBlock.setLed(!maskEnabled     ? LedState::Off
                        : maskVoices > 0  ? LedState::Active
                        :                   LedState::Idle);
    }

    // ── Sampler + engines: driven by their Enabled APVTS parameters ──────────
    samplerBlock.setLed(paramOn("luxSamplerEnabled") ? LedState::Active : LedState::Off);
    stralBlock  .setLed(paramOn("deviceEnabled")     ? LedState::Active : LedState::Off);
    synthBlock  .setLed(paramOn("luxsynthEnabled")   ? LedState::Active : LedState::Off);
    waveBlock   .setLed(paramOn("luxwaveEnabled")    ? LedState::Active : LedState::Off);
}
