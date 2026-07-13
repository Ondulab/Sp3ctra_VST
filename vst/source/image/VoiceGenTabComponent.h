/**
 * @file VoiceGenTabComponent.h
 * @brief PLAY page for the VOICE block — type a text, synthesize it offline
 *        with a Piper neural voice (language auto-detected or picked) and
 *        encode the speech into a playable vocal spectrum.
 *
 * Fourth sibling of ScoreGenTabComponent / TimbreGenTabComponent /
 * MidiScoreGenTabComponent: same band geometry (CIS height, log-frequency
 * axis, PhonoPaper dB profile via scoregen::renderScore) and the same
 * audition path — the rendered page is loaded into the shared SCORE player
 * channel (LuxSampler::loadScoreFramesFromImage + uiPlayScore), driven by the
 * shared transport params (scoreLoop / scoreReverse / scoreSpeed /
 * scorePlaying). All state persists as JSON in apvts.state ("voiceGenState");
 * the synthesized WAV is cached in Application Support/Sp3ctra/voice_renders
 * so a restored session can replay without re-synthesizing.
 *
 * The whole take is rendered with writingSpeed = 0 (stretched to one page):
 * text is NEVER cut, whatever its length; the log prints the Speed-knob value
 * that restores the natural speech tempo.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../IconPaths.h"
#include "../midi/MidiLearnAttachment.h"
#include "../tts/PiperTts.h"
#include "../tts/VoiceGenJob.h"
#include "../tts/LanguageDetector.h"

class VoiceGenTabComponent : public juce::Component,
                             private juce::Timer
{
public:
    static constexpr uint32_t kAccentARGB = 0xffd06a9e;   // rose (VOICE identity)
    static constexpr int      kPreferredH = 520;

    explicit VoiceGenTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p)
    {
        voices = PiperTts::listVoices();
        restoreState();

        // ── Worker callbacks (fire on the worker → marshal to message thread) ─
        job.onProgress = [this](float pr)
        {
            juce::MessageManager::callAsync(
                [sp = juce::Component::SafePointer<VoiceGenTabComponent>(this), pr]
                { if (sp != nullptr) sp->progressValue = pr; });
        };
        job.onDone = [this](VoiceGenJob::Result r)
        {
            juce::MessageManager::callAsync(
                [sp = juce::Component::SafePointer<VoiceGenTabComponent>(this),
                 res = std::move(r)]() mutable
                { if (sp != nullptr) sp->onRenderFinished(std::move(res)); });
        };

        // ── Voice row ────────────────────────────────────────────────────────
        initLabel(voiceLabel, "Voice");
        rebuildVoiceCombo();
        voiceCombo.onChange = [this]
        {
            const int id = voiceCombo.getSelectedId();
            if (id == 1)
                autoMode = true;
            else if (id >= 2 && id - 2 < voices.size())
            {
                autoMode = false;
                const auto& v = voices.getReference(id - 2);
                selectedVoiceId = v.id;
                if (v.lang.isNotEmpty())
                    langPref.set(v.lang, v.id);   // explicit pick = AUTO preference
            }
            markDirty();
        };
        addAndMakeVisible(voiceCombo);

        rescanButton.setButtonText("Rescan");
        rescanButton.setTooltip("Rescan the installed voice bundles");
        rescanButton.onClick = [this]
        {
            voices = PiperTts::listVoices();
            rebuildVoiceCombo();
            logLabel.setText(juce::String(voices.size()) + " voice(s) installed",
                             juce::dontSendNotification);
            repaint();
        };
        addAndMakeVisible(rescanButton);

        folderButton.setButtonText("Voices...");
        folderButton.setTooltip("Open the voices folder "
                                "(fill it with scripts/install_piper_voices.sh)");
        folderButton.onClick = []
        {
            auto dir = PiperTts::voicesDirectory();
            dir.createDirectory();
            dir.revealToUser();
        };
        addAndMakeVisible(folderButton);

        langBadge.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        langBadge.setColour(juce::Label::textColourId, juce::Colour(kAccentARGB));
        langBadge.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(langBadge);

        // ── Text ─────────────────────────────────────────────────────────────
        textEditor.setMultiLine(true, true);
        textEditor.setReturnKeyStartsNewLine(true);
        textEditor.setFont(juce::FontOptions(15.0f));
        textEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff10131a));
        textEditor.setColour(juce::TextEditor::textColourId,       juce::Colour(0xffd8dee8));
        textEditor.setColour(juce::TextEditor::outlineColourId,    juce::Colour(0xff33373f));
        textEditor.setColour(juce::TextEditor::focusedOutlineColourId,
                             juce::Colour(kAccentARGB).withAlpha(0.6f));
        textEditor.setTextToShowWhenEmpty("Type the text to speak...",
                                          juce::Colour(0xff55606f));
        textEditor.setText(text, juce::dontSendNotification);
        textEditor.onTextChange = [this] { text = textEditor.getText(); markDirty(); };
        addAndMakeVisible(textEditor);

        // ── Synthesis options ────────────────────────────────────────────────
        auto initRotary = [this](juce::Slider& s, juce::Label& l, const char* name,
                                 double lo, double hi, double def, const char* suffix)
        {
            initLabel(l, name);
            s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 52, 14);
            s.setColour(juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
            s.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
            s.setColour(juce::Slider::textBoxTextColourId,       juce::Colour(0xffa0c4e8));
            s.setRange(lo, hi, 0.01);
            s.setValue(def, juce::dontSendNotification);
            if (suffix != nullptr) s.setTextValueSuffix(suffix);
            s.onValueChange = [this] { markDirty(); };
            addAndMakeVisible(s);
        };
        initRotary(rateSlider,    rateLabel,    "Rate",       0.5, 2.0,  rate,    "x");
        rateSlider.setSkewFactorFromMidPoint(1.0);
        rateSlider.setTooltip("Speech rate (1x = the voice's natural pace)");
        initRotary(exprSlider,    exprLabel,    "Expression", 0.0, 1.5,  expr,    nullptr);
        exprSlider.setTooltip("VITS noise scale — 0 = flat/robotic, higher = livelier "
                              "(reloads the voice on next GENERATE)");
        initRotary(silenceSlider, silenceLabel, "Silence",    0.25, 3.0, silence, "x");
        silenceSlider.setTooltip("Scales the silence between sentences");
        rateSlider.onValueChange    = [this] { rate    = rateSlider.getValue();    markDirty(); };
        exprSlider.onValueChange    = [this] { expr    = exprSlider.getValue();    markDirty(); };
        silenceSlider.onValueChange = [this] { silence = silenceSlider.getValue(); markDirty(); };

        // ── Generate ─────────────────────────────────────────────────────────
        generateButton.setButtonText("GENERATE");
        generateButton.onClick = [this] { startGenerate(); };
        addAndMakeVisible(generateButton);

        progressBar = std::make_unique<juce::ProgressBar>(progressValue);
        addChildComponent(*progressBar);

        // ── Audition transport (shared SCORE player channel) ─────────────────
        playStopButton.setTooltip("Play / stop the voice through the score player");
        playStopButton.onClick = [this] { togglePlay(); };
        addAndMakeVisible(playStopButton);

        loopBtn.setTooltip("Loop playback");
        addAndMakeVisible(loopBtn);
        loopAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.getAPVTS(), "scoreLoop", loopBtn);

        reverseBtn.setTooltip("Reverse (play the take backward)");
        addAndMakeVisible(reverseBtn);
        reverseAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.getAPVTS(), "scoreReverse", reverseBtn);

        initLabel(speedLabel, "Speed");
        speedSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        speedSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 52, 14);
        speedSlider.setColour(juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
        speedSlider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        speedSlider.setColour(juce::Slider::textBoxTextColourId,       juce::Colour(0xffa0c4e8));
        speedSlider.setRange(0.1, 6.0, 0.01);
        speedSlider.setTextValueSuffix("x");
        speedSlider.setSkewFactorFromMidPoint(1.0);
        addAndMakeVisible(speedSlider);
        speedAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor.getAPVTS(), "scoreSpeed", speedSlider);

        // Right-click MIDI Learn on the shared SCORE transport params.
        {
            auto& mm = processor.getMidiMap();
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, playStopButton, "scorePlaying"));
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, loopBtn,        "scoreLoop"));
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, reverseBtn,     "scoreReverse"));
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, speedSlider,    "scoreSpeed"));
        }

        playHint.setText("PLAY loads the voice into the score player "
                         "(set LuxStral source = Sampler to hear it).",
                         juce::dontSendNotification);
        playHint.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        playHint.setColour(juce::Label::textColourId, juce::Colour(0xff8890a0));
        addAndMakeVisible(playHint);

        // ── Log ──────────────────────────────────────────────────────────────
        logLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        logLabel.setColour(juce::Label::textColourId, juce::Colour(0xff8890a0));
        logLabel.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(logLabel);

        if (! PiperTts::isEngineAvailable())
            logLabel.setText("TTS engine not available in this build "
                             "(SP3CTRA_ENABLE_TTS=OFF)", juce::dontSendNotification);
        else if (voices.isEmpty())
            logLabel.setText("No voices installed — run scripts/install_piper_voices.sh "
                             "then Rescan", juce::dontSendNotification);

        startTimerHz(20);
    }

    ~VoiceGenTabComponent() override
    {
        stopTimer();
        if (stateDirty)
            persistState();
        // Like SCORE/TIMBRE/MIDI SCORE: the page is a VIEW — closing it must
        // not cut audio playing through the shared channel.
    }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        const juce::Colour accent(kAccentARGB);

        g.setColour(juce::Colour(0xff10131a));
        g.fillRect(previewArea);
        g.setColour(accent.withAlpha(0.35f));
        g.drawRect(previewArea, 1);

        if (previewImage.isValid())
        {
            const auto imgArea = previewImageBounds();
            previewImgArea = imgArea;
            g.setOpacity(1.0f);
            g.drawImage(previewImage, imgArea);

            // Reading head — only while OUR frames sit in the player.
            auto* fs = processor.getLuxSampler();
            if (fs != nullptr && framesAreOurs)
            {
                const bool playing = fs->isScorePlaying();
                int headFrame = -1;
                if (playing)             headFrame = fs->getScorePlayHead();
                else if (scrubHead >= 0) headFrame = scrubHead;
                if (headFrame >= 0)
                {
                    const int n = juce::jmax(1, fs->getScoreFrameCount());
                    const float frac = juce::jlimit(0.f, 1.f, (float) headFrame / (float) n);
                    const float lx = imgArea.getX() + frac * imgArea.getWidth();
                    g.setColour(accent.withAlpha(playing ? 0.9f : 0.6f));
                    g.fillRect(lx - 0.75f, imgArea.getY(), 1.5f, imgArea.getHeight());
                }
            }
        }
        else
        {
            g.setColour(juce::Colour(0xff55606f));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
            juce::String hint;
            if (! PiperTts::isEngineAvailable())
                hint = "TTS engine not available (build with SP3CTRA_ENABLE_TTS=ON)";
            else if (voices.isEmpty())
                hint = "No voices installed — run scripts/install_piper_voices.sh";
            else if (busy)
                hint = "Generating...";
            else
                hint = "Type a text and GENERATE its vocal spectrum";
            g.drawText(hint, previewArea, juce::Justification::centred);
        }
    }

    //==========================================================================
    // Preview interactions: click = scrub (same behaviour as the siblings).
    void mouseDown(const juce::MouseEvent& e) override
    {
        scrubbing = framesAreOurs && previewImage.isValid()
                 && previewArea.contains(e.getPosition());
        if (! scrubbing) return;
        scrubTo(e);
        if (auto* fs = processor.getLuxSampler())
            if (! fs->isScorePlaying())
                scrubAuditioning = fs->uiBeginScoreScrub();
    }
    void mouseDrag(const juce::MouseEvent& e) override { if (scrubbing) scrubTo(e); }
    void mouseUp  (const juce::MouseEvent&)   override
    {
        scrubbing = false;
        if (scrubAuditioning)
        {
            if (auto* fs = processor.getLuxSampler()) fs->uiEndScoreScrub();
            scrubAuditioning = false;
        }
    }

    void resized() override
    {
        const int pad = 8;
        const int ch  = Sp3ctraTheme::kControlH;
        const int gap = 6;
        const int w   = getWidth();

        // ── Voice row ────────────────────────────────────────────────────────
        int y = pad;
        voiceLabel.setBounds(pad, y, 40, ch);
        voiceCombo.setBounds(pad + 40 + gap, y, 240, ch);
        rescanButton.setBounds(pad + 40 + gap + 240 + gap, y, 64, ch);
        folderButton.setBounds(pad + 40 + gap + 240 + gap + 64 + gap, y, 72, ch);
        langBadge.setBounds(pad + 40 + gap + 240 + gap + 64 + gap + 72 + gap, y,
                            juce::jmax(60, w - (pad * 2 + 40 + 240 + 64 + 72 + gap * 4)), ch);
        y += ch + gap;

        // ── Text editor ──────────────────────────────────────────────────────
        textEditor.setBounds(pad, y, w - pad * 2, 64);
        y += 64 + gap;

        // ── Options + generate + transport, one row ──────────────────────────
        const int knobW = 72, knobH = ch * 2 + 16;
        int x = pad;
        auto knob = [&](juce::Label& l, juce::Slider& s)
        {
            l.setBounds(x, y, knobW, 14);
            s.setBounds(x, y + 14, knobW, knobH - 14);
            x += knobW + gap;
        };
        knob(rateLabel,    rateSlider);
        knob(exprLabel,    exprSlider);
        knob(silenceLabel, silenceSlider);

        x += gap;
        generateButton.setBounds(x, y + 14, 110, ch + 6);
        progressBar->setBounds(x, y + 14 + ch + 10, 110, 12);
        x += 110 + gap * 3;

        const int tBtn = ch + 6;
        playStopButton.setBounds(x, y + 14, tBtn, tBtn);          x += tBtn + gap;
        loopBtn.setBounds(x, y + 14, tBtn, tBtn);                 x += tBtn + gap;
        reverseBtn.setBounds(x, y + 14, tBtn, tBtn);              x += tBtn + gap;
        speedLabel.setBounds(x, y, knobW, 14);
        speedSlider.setBounds(x, y + 14, knobW, knobH - 14);      x += knobW + gap;
        playHint.setBounds(x, y + 14, juce::jmax(60, w - pad - x), knobH - 14);
        y += knobH + gap;

        // ── Log + preview ────────────────────────────────────────────────────
        logLabel.setBounds(pad, y, w - pad * 2, 28);
        y += 28 + gap;
        previewArea = juce::Rectangle<int>(pad, y, w - pad * 2,
                                           juce::jmax(80, getHeight() - pad - y));
    }

private:
    //==========================================================================
    /** Square play/stop transport button (same visual language as SCORE's). */
    class VoicePlayButton : public juce::Button
    {
    public:
        VoicePlayButton() : juce::Button("voicePlayStop") {}

        void setPlaying(bool p)
        {
            if (p == playing) return;
            playing = p;
            repaint();
        }

        void paintButton(juce::Graphics& g, bool over, bool down) override
        {
            const auto b = getLocalBounds().toFloat().reduced(1.f);
            const juce::Colour bg(0xff222230);
            g.setColour(down ? bg.brighter(0.30f) : over ? bg.brighter(0.12f) : bg);
            g.fillRoundedRectangle(b, 3.f);
            g.setColour(juce::Colour(0xff33373f));
            g.drawRoundedRectangle(b, 3.f, 1.f);

            const auto inner = b.reduced(b.getHeight() * 0.30f);
            if (! isEnabled())
                Icons::fillPath(g, Icons::play(), inner, juce::Colour(0xff555a62));
            else if (playing)
                Icons::fillPath(g, Icons::stop(), inner, juce::Colour(kAccentARGB));
            else
                Icons::fillPath(g, Icons::play(), inner, juce::Colour(0xff66cc88));
        }

    private:
        bool playing = false;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoicePlayButton)
    };

    /** Compact loop/inverse pictogram toggle — same glyph as the SCORE page,
     *  only the accent differs. */
    class VoiceIconToggle : public juce::Button
    {
    public:
        enum class Glyph { Loop, Inverse };

        explicit VoiceIconToggle(Glyph g) : juce::Button("voiceIconToggle"), glyph(g)
        {
            setClickingTogglesState(true);
        }

        void paintButton(juce::Graphics& g, bool over, bool down) override
        {
            const auto b = getLocalBounds().toFloat().reduced(1.f);
            const bool on = getToggleState() && isEnabled();
            const juce::Colour accent(kAccentARGB);

            const juce::Colour bg = on ? accent.withAlpha(0.22f) : juce::Colour(0xff222230);
            g.setColour(down ? bg.brighter(0.30f) : over ? bg.brighter(0.12f) : bg);
            g.fillRoundedRectangle(b, 3.f);
            g.setColour(on ? accent.withAlpha(0.9f) : juce::Colour(0xff33373f));
            g.drawRoundedRectangle(b, 3.f, 1.f);

            const auto inner = b.reduced(b.getHeight() * 0.22f);
            const juce::Colour fg = on ? accent
                                       : juce::Colour(isEnabled() ? 0xff9aa6ba : 0xff555a62);
            drawLoopGlyph(g, inner, fg, glyph == Glyph::Inverse);
        }

    private:
        /** Stadium (racetrack) loop, open at the top, arrow capping the gap —
         *  shared shape with the SCORE / TIMBRE / MIDI SCORE toggles. */
        static void drawLoopGlyph(juce::Graphics& g, juce::Rectangle<float> r,
                                  juce::Colour col, bool reversed)
        {
            const float h  = r.getHeight();
            const float th = juce::jmax(2.0f, h * 0.12f);   // stroke thickness

            const float ringH  = h * 0.64f;
            const float L = r.getX() + th * 0.6f;
            const float R = r.getRight() - th * 0.6f;
            const float T = r.getCentreY() - ringH * 0.5f;
            const float B = r.getCentreY() + ringH * 0.5f;
            const float radius = (B - T) * 0.5f;
            const float midY   = (T + B) * 0.5f;
            const float topLx  = L + radius;
            const float topRx  = R - radius;
            const float gx0    = juce::jmap(0.34f, topLx, topRx);
            const float gx1    = juce::jmap(0.66f, topLx, topRx);

            juce::Path loop;
            loop.startNewSubPath(gx1, T);
            loop.lineTo(topRx, T);
            loop.addCentredArc(topRx, midY, radius, radius, 0.0f,
                               0.0f, juce::MathConstants<float>::pi, false);
            loop.lineTo(topLx, B);
            loop.addCentredArc(topLx, midY, radius, radius, 0.0f,
                               juce::MathConstants<float>::pi,
                               juce::MathConstants<float>::twoPi, false);
            loop.lineTo(gx0, T);

            const float aH     = radius * 0.85f;
            const float aTipX  = gx0 - th * 0.25f;
            const float aBackX = gx1 + th * 0.25f;
            juce::Path arrow;
            arrow.addTriangle(aTipX, T, aBackX, T - aH, aBackX, T + aH);

            const auto ringBounds = loop.getBounds().expanded(th * 0.5f);
            const auto offset = r.getCentre() - ringBounds.getCentre();
            const auto move = juce::AffineTransform::translation(offset.x, offset.y);
            loop.applyTransform(move);
            arrow.applyTransform(move);

            if (reversed)
            {
                const auto flip = juce::AffineTransform::scale(-1.0f, 1.0f)
                                      .translated(r.getCentreX() * 2.0f, 0.0f);
                loop.applyTransform(flip);
                arrow.applyTransform(flip);
            }

            g.setColour(col);
            g.strokePath(loop, juce::PathStrokeType(th, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
            g.fillPath(arrow);
        }

        Glyph glyph;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceIconToggle)
    };

    //==========================================================================
    void initLabel(juce::Label& l, const char* txt)
    {
        l.setText(txt, juce::dontSendNotification);
        l.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        l.setColour(juce::Label::textColourId, juce::Colour(0xff8890a0));
        addAndMakeVisible(l);
    }

    void markDirty()
    {
        stateDirty = true;
        lastEditMs = juce::Time::getMillisecondCounter();
    }

    static juce::File cacheWavFile()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("Application Support/Sp3ctra/voice_renders/voice_last.wav");
    }

    bool modelHasVoice() const
    {
        for (const auto& ch : processor.getChainModel().chains)
            for (const auto& m : ch.modules)
                if (m.type == ModuleType::Voice)
                    return true;
        return false;
    }

    void rebuildVoiceCombo()
    {
        voiceCombo.clear(juce::dontSendNotification);
        voiceCombo.addItem("AUTO (detect language)", 1);
        for (int i = 0; i < voices.size(); ++i)
            voiceCombo.addItem(voices.getReference(i).displayName(), 2 + i);

        int select = 1;
        if (! autoMode)
            for (int i = 0; i < voices.size(); ++i)
                if (voices.getReference(i).id == selectedVoiceId)
                    { select = 2 + i; break; }
        voiceCombo.setSelectedId(select, juce::dontSendNotification);
        autoMode = (select == 1);
    }

    /** ScoreSettings for the vocal spectrum: PhonoPaper defaults, whole take on
     *  one A3-landscape page (never cut), frequency span = the instrument's. */
    ScoreSettings makeScoreSettings() const
    {
        ScoreSettings s;
        score_settings_defaults(&s);
        s.pageFormat    = 1;       // A3 landscape — widest band, best time detail
        s.writingSpeed  = 0.0;     // whole take stretched to the page width
        s.binsPerSecond = 300.0;   // 3.3 ms columns: consonants stay crisp
        double lo = 0.0, hi = 0.0;
        processor.getScoreFrequencyRange(lo, hi);
        s.minFreq = lo;
        s.maxFreq = hi;
        return s;
    }

    /** AUTO mode: detected language → preferred (last picked) voice for it,
     *  else best installed match (medium > high > low; en → US then GB). */
    bool resolveAutoVoice(const juce::String& forText, PiperVoiceInfo& out,
                          juce::String& outLang)
    {
        juce::StringArray langs;
        for (const auto& v : voices)
            if (v.lang.isNotEmpty())
                langs.addIfNotAlreadyThere(v.lang);

        outLang = LanguageDetector::detect(forText, langs);

        auto findById = [this](const juce::String& id) -> const PiperVoiceInfo*
        {
            for (const auto& v : voices)
                if (v.id == id) return &v;
            return nullptr;
        };

        if (outLang.isEmpty())
        {
            // Undecided (short text…): last explicit voice, else first installed.
            if (const auto* v = findById(selectedVoiceId)) { out = *v; outLang = v->lang; return true; }
            if (! voices.isEmpty()) { out = voices.getReference(0); outLang = out.lang; return true; }
            return false;
        }

        if (const auto* pref = langPref.getVarPointer(juce::Identifier(outLang)))
            if (const auto* v = findById(pref->toString()))
                { out = *v; return true; }

        const PiperVoiceInfo* best = nullptr;
        auto qualityRank = [](const juce::String& q)
        { return q == "medium" ? 0 : q == "high" ? 1 : 2; };
        auto regionRank = [&](const juce::String& r)
        { return (outLang == "en") ? (r == "US" ? 0 : r == "GB" ? 1 : 2) : 0; };
        for (const auto& v : voices)
        {
            if (v.lang != outLang) continue;
            if (best == nullptr
                || qualityRank(v.quality) < qualityRank(best->quality)
                || (qualityRank(v.quality) == qualityRank(best->quality)
                    && regionRank(v.region) < regionRank(best->region)))
                best = &v;
        }
        if (best != nullptr) { out = *best; return true; }

        // Detected a language with no installed voice — fall back like undecided.
        if (const auto* v = findById(selectedVoiceId)) { out = *v; return true; }
        if (! voices.isEmpty()) { out = voices.getReference(0); return true; }
        return false;
    }

    //==========================================================================
    void startGenerate()
    {
        if (busy) return;
        if (! PiperTts::isEngineAvailable())
        {
            logLabel.setText("TTS engine not available in this build "
                             "(SP3CTRA_ENABLE_TTS=OFF)", juce::dontSendNotification);
            return;
        }
        const juce::String t = textEditor.getText().trim();
        if (t.isEmpty())
        {
            logLabel.setText("Type some text first", juce::dontSendNotification);
            return;
        }
        if (voices.isEmpty())
        {
            logLabel.setText("No voices installed — run scripts/install_piper_voices.sh "
                             "then Rescan", juce::dontSendNotification);
            return;
        }

        PiperVoiceInfo v;
        juce::String lang;
        const juce::String arrow = juce::String::fromUTF8(" \xe2\x86\x92 ");
        if (autoMode)
        {
            if (! resolveAutoVoice(t, v, lang))
                return;
            langBadge.setText(lang + arrow + v.name, juce::dontSendNotification);
        }
        else
        {
            bool found = false;
            for (const auto& vi : voices)
                if (vi.id == selectedVoiceId) { v = vi; found = true; break; }
            if (! found) v = voices.getReference(0);
            langBadge.setText(v.lang + arrow + v.name, juce::dontSendNotification);
        }

        VoiceGenJob::Request r;
        r.text  = t;
        r.voice = v;
        r.opts.lengthScale          = (float) (1.0 / juce::jlimit(0.25, 4.0, rate));
        r.opts.noiseScale           = (float) expr;
        r.opts.sentenceSilenceScale = (float) silence;
        r.score   = makeScoreSettings();
        r.wavFile = cacheWavFile();
        genMinFreq = r.score.minFreq;
        genMaxFreq = r.score.maxFreq;

        beginJobUi();
        if (auto* fs = processor.getLuxSampler())
            if (framesAreOurs)
                fs->uiStopScore();
        job.start(std::move(r));
    }

    /** Session-restore replay: re-encode the cached WAV without re-synthesis. */
    void startRenderOnly(bool autoPlayWhenDone)
    {
        if (busy) return;
        VoiceGenJob::Request r;
        r.renderOnly = true;
        r.score      = makeScoreSettings();
        r.wavFile    = cacheWavFile();
        genMinFreq   = r.score.minFreq;
        genMaxFreq   = r.score.maxFreq;
        pendingAutoPlay = autoPlayWhenDone;

        beginJobUi();
        job.start(std::move(r));
    }

    void beginJobUi()
    {
        busy = true;
        progressValue = 0.0;
        generateButton.setEnabled(false);
        progressBar->setVisible(true);
        logLabel.setText("Generating...", juce::dontSendNotification);
        repaint(previewArea);
    }

    void onRenderFinished(VoiceGenJob::Result r)
    {
        busy = false;
        progressBar->setVisible(false);
        generateButton.setEnabled(true);

        if (! r.ok())
        {
            pendingAutoPlay = false;
            const juce::String why = r.error.isNotEmpty() ? r.error : r.render.log;
            logLabel.setText("Failed: " + why, juce::dontSendNotification);
            repaint();
            return;
        }

        generatedImage = r.render.image;
        spectroBand    = r.render.spectroBand;
        audioSeconds   = r.audioSeconds;
        buildPreview();
        reloadPlayFrames();

        // Natural speech tempo: the player injects 1000 columns/s at Speed 1x,
        // the band holds `frames` columns for `audioSeconds` of speech.
        juce::String msg = juce::String(loadedFrameCount) + " frames from "
                         + juce::String(audioSeconds, 2) + " s of speech";
        if (audioSeconds > 0.0 && loadedFrameCount > 0)
            msg += " - natural tempo at Speed "
                 + juce::String((double) loadedFrameCount / (audioSeconds * 1000.0), 2) + "x";
        if (r.voiceId.isNotEmpty())
            msg += " - " + r.voiceId;
        logLabel.setText(msg, juce::dontSendNotification);

        if (pendingAutoPlay)
        {
            pendingAutoPlay = false;
            if (auto* p = processor.getAPVTS().getParameter("scorePlaying"))
                if (p->getValue() < 0.5f)
                    p->setValueNotifyingHost(1.0f);
        }

        persistState();   // WAV cache path / last take
        repaint();
    }

    void buildPreview()
    {
        if (! generatedImage.isValid())
        {
            previewImage = juce::Image();
            return;
        }
        // Preview only the spectrogram band (what actually plays) — the white
        // page margins would wash the thumbnail grey. Downscale once here so
        // the 20 Hz head repaint stays cheap.
        juce::Rectangle<int> band =
            (spectroBand.getWidth() > 0 && spectroBand.getHeight() > 0)
                ? spectroBand.getIntersection(generatedImage.getBounds())
                : generatedImage.getBounds();
        if (band.isEmpty())
            band = generatedImage.getBounds();

        juce::Image cropped = generatedImage.getClippedImage(band);
        constexpr int kMaxPreviewW = 2048;
        if (cropped.getWidth() > kMaxPreviewW)
            previewImage = cropped.rescaled(
                kMaxPreviewW,
                juce::jmax(1, cropped.getHeight() * kMaxPreviewW / cropped.getWidth()));
        else
            previewImage = cropped.createCopy();
    }

    juce::Rectangle<float> previewImageBounds() const
    {
        if (! previewImage.isValid() || previewArea.isEmpty())
            return {};
        const juce::Rectangle<float> dest(
            (float) previewArea.getX() + 2, (float) previewArea.getY() + 2,
            (float) previewArea.getWidth() - 4, (float) previewArea.getHeight() - 4);
        const juce::RectanglePlacement place(juce::RectanglePlacement::centred);
        return place.appliedTo(
            juce::Rectangle<float>(0.f, 0.f,
                (float) previewImage.getWidth(), (float) previewImage.getHeight()),
            dest);
    }

    //==========================================================================
    bool reloadPlayFrames()
    {
        auto* fs = processor.getLuxSampler();
        if (fs == nullptr || ! generatedImage.isValid())
            return false;

        fs->loadScoreFramesFromImage(generatedImage, spectroBand,
                                     genMinFreq, genMaxFreq, false);
        framesAreOurs    = true;
        loadedFrameCount = fs->getScoreFrameCount();
        scrubHead        = -1;
        return true;
    }

    void togglePlay()
    {
        auto* fs = processor.getLuxSampler();
        if (fs == nullptr || busy) return;

        const bool play = ! (fs->isScorePlaying() && framesAreOurs);

        if (play && ! framesAreOurs)
        {
            if (generatedImage.isValid())
            {
                // Another module took the shared channel — reclaim it.
                if (! reloadPlayFrames())
                    return;
            }
            else if (cacheWavFile().existsAsFile())
            {
                // Restored session: re-encode the cached WAV, then auto-play.
                startRenderOnly(true);
                return;
            }
            else
            {
                logLabel.setText("GENERATE first", juce::dontSendNotification);
                return;
            }
        }
        scrubHead = -1;

        // Route through the scorePlaying param so the DAW lane stays truthful
        // (same path as the SCORE page — shared transport, shared channel).
        if (auto* p = processor.getAPVTS().getParameter("scorePlaying"))
        {
            const float norm = play ? 1.0f : 0.0f;
            if (! juce::approximatelyEqual(p->getValue(), norm))
                p->setValueNotifyingHost(norm);
            else if (play) fs->uiPlayScore();
            else           fs->uiStopScore();
        }
        playStopButton.setPlaying(play);
        repaint(previewArea);
    }

    void scrubTo(const juce::MouseEvent& e)
    {
        auto* fs = processor.getLuxSampler();
        if (fs == nullptr || ! framesAreOurs) return;
        const int n = fs->getScoreFrameCount();
        if (n <= 0) return;
        const auto area = previewImgArea.isEmpty() ? previewImageBounds() : previewImgArea;
        if (area.getWidth() <= 0.f) return;
        const float fx = juce::jlimit(0.f, 1.f,
            ((float) e.position.x - area.getX()) / area.getWidth());
        scrubHead = juce::jlimit(0, n - 1, (int) (fx * (float) n));
        fs->uiSeekScore(scrubHead);
        repaint(previewArea);
    }

    void timerCallback() override
    {
        // Debounced persistence (typing-friendly).
        if (stateDirty
            && juce::Time::getMillisecondCounter() - lastEditMs > 800)
            persistState();

        // Progress repaint while the worker runs.
        if (busy)
        {
            progressBar->repaint();
            return;
        }

        auto* fs = processor.getLuxSampler();
        if (fs != nullptr && framesAreOurs)
        {
            // Ownership check: SCORE/TIMBRE/MIDI SCORE (or a restore) reloaded
            // the shared channel → our head display no longer applies.
            if (fs->getScoreFrameCount() != loadedFrameCount)
            {
                framesAreOurs = false;
                scrubHead     = -1;
                repaint(previewArea);
            }
            else
            {
                playStopButton.setPlaying(fs->isScorePlaying());
                if (fs->isScorePlaying())
                    repaint(previewArea);
            }
        }
        else
            playStopButton.setPlaying(false);

        // Voice-model RAM housekeeping: the module left the chain model →
        // release the resident engine (~200 MB) once idle.
        if (++idleTicks >= 100)   // every ~5 s at 20 Hz
        {
            idleTicks = 0;
            if (job.engineLoaded() && ! modelHasVoice())
                job.unloadEngine();
        }
    }

    //==========================================================================
    // Persistence — one JSON blob in apvts.state (like midiScoreGenState).
    void persistState()
    {
        stateDirty = false;
        auto* root = new juce::DynamicObject();
        root->setProperty("text",  text);
        root->setProperty("voice", autoMode ? juce::String("auto") : selectedVoiceId);
        root->setProperty("rate",  rate);
        root->setProperty("expr",  expr);
        root->setProperty("sil",   silence);
        auto* prefs = new juce::DynamicObject();
        for (int i = 0; i < langPref.size(); ++i)
            prefs->setProperty(langPref.getName(i), langPref.getValueAt(i));
        root->setProperty("pref", juce::var(prefs));
        processor.getAPVTS().state.setProperty(
            "voiceGenState", juce::JSON::toString(juce::var(root), true), nullptr);
    }

    void restoreState()
    {
        const juce::String blob = processor.getAPVTS().state
            .getProperty("voiceGenState", "").toString();
        if (blob.isEmpty()) return;
        const juce::var root = juce::JSON::parse(blob);
        auto* o = root.getDynamicObject();
        if (o == nullptr) return;

        text = o->getProperty("text").toString();
        const juce::String v = o->getProperty("voice").toString();
        autoMode        = (v.isEmpty() || v == "auto");
        selectedVoiceId = autoMode ? juce::String() : v;
        if (o->hasProperty("rate")) rate    = (double) o->getProperty("rate");
        if (o->hasProperty("expr")) expr    = (double) o->getProperty("expr");
        if (o->hasProperty("sil"))  silence = (double) o->getProperty("sil");
        if (auto* prefs = o->getProperty("pref").getDynamicObject())
            langPref = prefs->getProperties();
    }

    //==========================================================================
    Sp3ctraAudioProcessor& processor;

    juce::Array<PiperVoiceInfo> voices;
    VoiceGenJob job;

    // Persisted page state.
    juce::String text;
    bool         autoMode = true;
    juce::String selectedVoiceId;
    double       rate = 1.0, expr = 0.667, silence = 1.0;
    juce::NamedValueSet langPref;          // lang → last explicitly picked voice

    // Last generation.
    juce::Image          generatedImage;
    juce::Rectangle<int> spectroBand;
    double genMinFreq = 0.0, genMaxFreq = 0.0;
    double audioSeconds = 0.0;

    juce::Label    voiceLabel, langBadge, rateLabel, exprLabel, silenceLabel,
                   speedLabel, playHint, logLabel;
    juce::ComboBox voiceCombo;
    juce::TextButton rescanButton, folderButton, generateButton;
    juce::TextEditor textEditor;
    juce::Slider   rateSlider, exprSlider, silenceSlider, speedSlider;
    VoicePlayButton playStopButton;
    VoiceIconToggle loopBtn    { VoiceIconToggle::Glyph::Loop };
    VoiceIconToggle reverseBtn { VoiceIconToggle::Glyph::Inverse };
    std::unique_ptr<juce::ProgressBar> progressBar;
    double progressValue = 0.0;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> loopAttach, reverseAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> speedAttach;
    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;

    juce::Rectangle<int>   previewArea;
    juce::Rectangle<float> previewImgArea;
    juce::Image previewImage;

    bool busy = false, pendingAutoPlay = false, stateDirty = false;
    juce::uint32 lastEditMs = 0;
    int idleTicks = 0;

    bool framesAreOurs = false;
    int  loadedFrameCount = 0;
    int  scrubHead = -1;
    bool scrubbing = false, scrubAuditioning = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceGenTabComponent)
};
