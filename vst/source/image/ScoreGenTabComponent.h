/**
 * @file ScoreGenTabComponent.h
 * @brief PLAY page for the SCORE block — generate a printable greyscale
 *        spectrogram ("graphical score") from a WAV file and export it as
 *        PNG/JPEG.
 *
 * Port of the legacy Sp3ctraGen app (audio→spectrogram) into the VST shell:
 *   Load WAV → tweak parameters → GENERATE (worker thread) → preview → EXPORT.
 *
 * Parameters live in a local ScoreSettings (offline export — never read by
 * processBlock, so deliberately NOT in the APVTS / not host-automatable).
 *
 * Beyond export, the generated image can be PLAYED back through the synthesis
 * pipeline: the SCORE block now sits in CHAIN 1 (between SAMPLER and LUXSTRAL)
 * and reuses the LuxSampler playback engine. The Play/Stop/Loop/Speed transport
 * below drives LuxSampler::uiPlayScore()/uiStopScore().
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "ScoreGenThread.h"
#include "ScoreGenRenderer.h"

class ScoreGenTabComponent : public juce::Component,
                             private juce::Timer
{
public:
    static constexpr uint32_t kAccentARGB = 0xffe0a24a;   // amber (SCORE identity)

    explicit ScoreGenTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p)
    {
        const juce::Colour accent(kAccentARGB);

        // ── Load WAV ───────────────────────────────────────────────────────
        // Generation parameters now live on the SETUP face (ScoreSetupPanel) —
        // this PLAY page keeps only the load/generate/export/transport actions.
        loadButton.setButtonText("Load WAV...");
        loadButton.onClick = [this] { chooseWav(); };
        addAndMakeVisible(loadButton);

        fileLabel.setText("No file loaded", juce::dontSendNotification);
        fileLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        fileLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb8c0d0));
        addAndMakeVisible(fileLabel);

        // ── Writing Speed (essential — maps audio duration to page width) ────
        initLabel(wsLabel, "Writing Speed (cm/s)");
        initSlider(wsSlider, 0.5, 10.0, 0.1, processor.getScoreSettings().writingSpeed);
        wsSlider.onValueChange = [this]
        { processor.getScoreSettings().writingSpeed = wsSlider.getValue(); };

        // ── Generate + progress ────────────────────────────────────────────
        generateButton.setButtonText("GENERATE");
        generateButton.onClick = [this] { startGenerate(); };
        addAndMakeVisible(generateButton);

        progressBar.setPercentageDisplay(true);
        addChildComponent(progressBar);

        // ── Export ─────────────────────────────────────────────────────────
        exportPngButton.setButtonText("Export PNG");
        exportPngButton.onClick = [this] { chooseExport(true); };
        exportPngButton.setEnabled(false);
        addAndMakeVisible(exportPngButton);

        exportJpgButton.setButtonText("Export JPEG");
        exportJpgButton.onClick = [this] { chooseExport(false); };
        exportJpgButton.setEnabled(false);
        addAndMakeVisible(exportJpgButton);

        // ── Playback transport (CHAIN 1) ───────────────────────────────────
        playStopButton.setButtonText("PLAY");
        playStopButton.setEnabled(false);
        playStopButton.onClick = [this] { togglePlay(); };
        addAndMakeVisible(playStopButton);

        loopToggle.setButtonText("Loop");
        loopToggle.setToggleState(true, juce::dontSendNotification);
        loopToggle.setEnabled(false);
        loopToggle.onClick = [this]
        {
            if (auto* fs = processor.getLuxSampler())
                fs->setScoreLoopMode(loopToggle.getToggleState() ? LoopMode::LOOP
                                                                 : LoopMode::NONE);
        };
        addAndMakeVisible(loopToggle);

        initLabel(speedLabel, "Speed");
        initSlider(speedSlider, 0.1, 8.0, 0.01, 1.0);
        speedSlider.setEnabled(false);
        speedSlider.onValueChange = [this]
        {
            if (auto* fs = processor.getLuxSampler())
                fs->setScoreSpeed((float) speedSlider.getValue());
        };

        playHint.setText("Set LuxStral source = Sampler to hear the score.",
                         juce::dontSendNotification);
        playHint.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        playHint.setColour(juce::Label::textColourId, juce::Colour(0xff8890a0));
        addAndMakeVisible(playHint);

        startTimerHz(10);   // reflect auto-stop (LoopMode::NONE end) on the button

        // ── Log ────────────────────────────────────────────────────────────
        logLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        logLabel.setColour(juce::Label::textColourId, juce::Colour(0xff8890a0));
        logLabel.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(logLabel);

        // Worker callbacks marshal back to the message thread.
        job.onProgress = [safe = juce::Component::SafePointer<ScoreGenTabComponent>(this)]
                         (float pr)
        {
            juce::MessageManager::callAsync([safe, pr]
            {
                if (auto* self = safe.getComponent())
                    self->progress = (double) pr;
            });
        };
        job.onDone = [safe = juce::Component::SafePointer<ScoreGenTabComponent>(this)]
                     (scoregen::RenderResult r)
        {
            juce::MessageManager::callAsync([safe, r = std::move(r)]() mutable
            {
                if (auto* self = safe.getComponent())
                    self->onRenderFinished(std::move(r));
            });
        };

        juce::ignoreUnused(accent);
    }

    ~ScoreGenTabComponent() override
    {
        stopTimer();
        if (auto* fs = processor.getLuxSampler())
            fs->uiStopScore();
        job.stopThread(3000);
    }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        const juce::Colour accent(kAccentARGB);

        // Section header
        g.setColour(accent.withAlpha(0.85f));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText("SCORE - PRINTABLE SPECTROGRAM",
                   8, 4, getWidth() - 16, 14, juce::Justification::centredLeft);

        // Preview frame
        g.setColour(juce::Colour(0xff10131a));
        g.fillRect(previewArea);
        g.setColour(accent.withAlpha(0.35f));
        g.drawRect(previewArea, 1);

        if (previewImage.isValid())
        {
            const juce::Rectangle<float> dest(
                (float) previewArea.getX() + 2, (float) previewArea.getY() + 2,
                (float) previewArea.getWidth() - 4, (float) previewArea.getHeight() - 4);
            const juce::RectanglePlacement place(
                juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize);
            const auto imgArea = place.appliedTo(
                juce::Rectangle<float>(0.f, 0.f,
                    (float) previewImage.getWidth(), (float) previewImage.getHeight()),
                dest);
            // drawImage() modulates by the current fill's alpha; the 0.35 set
            // for the frame border above would otherwise blit the image at 35%
            // opacity over the dark frame (→ grey floor). Force full opacity.
            g.setOpacity(1.0f);
            g.drawImage(previewImage, imgArea);

            // ── Reading head: vertical line at the exact column being played ──
            auto* fs = processor.getLuxSampler();
            if (fs != nullptr && fs->isScorePlaying())
            {
                const int n = juce::jmax(1, fs->getScoreFrameCount());
                const float frac = juce::jlimit(0.f, 1.f, (float) fs->getScorePlayHead() / (float) n);
                const float lx = imgArea.getX() + frac * imgArea.getWidth();
                g.setColour(accent.withAlpha(0.9f));
                g.fillRect(lx - 0.75f, imgArea.getY(), 1.5f, imgArea.getHeight());
            }
        }
        else
        {
            g.setColour(juce::Colour(0xff55606f));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
            g.drawText(busy ? "Generating..." : "Load a WAV and press GENERATE",
                       previewArea, juce::Justification::centred);
        }
    }

    void resized() override
    {
        const int pad = 8;
        const int ch  = Sp3ctraTheme::kControlH;
        const int gap = 6;
        const int colW = juce::jmin(330, getWidth() - 2 * pad);
        int y = 24;

        loadButton.setBounds(pad, y, 110, ch);
        fileLabel.setBounds(pad + 110 + gap, y, colW - 110 - gap, ch);
        y += ch + gap + 8;

        {
            const int lblW = 150;
            wsLabel.setBounds(pad, y, lblW, ch);
            wsSlider.setBounds(pad + lblW + gap, y, colW - lblW - gap, ch);
            y += ch + gap + 4;
        }

        generateButton.setBounds(pad, y, colW, ch + 4); y += ch + 8;
        progressBar.setBounds(pad, y, colW, ch);        y += ch + gap;
        exportPngButton.setBounds(pad, y, (colW - gap) / 2, ch);
        exportJpgButton.setBounds(pad + (colW - gap) / 2 + gap, y, (colW - gap) / 2, ch);
        y += ch + gap + 6;

        // ── Playback transport ─────────────────────────────────────────────
        playStopButton.setBounds(pad, y, colW, ch + 4); y += ch + 8;
        loopToggle.setBounds(pad, y, 80, ch);
        speedLabel.setBounds(pad + 80 + gap, y, 54, ch);
        speedSlider.setBounds(pad + 80 + gap + 54 + gap, y,
                              colW - 80 - gap - 54 - gap, ch);
        y += ch + gap;
        playHint.setBounds(pad, y, colW, ch); y += ch + gap + 4;

        logLabel.setBounds(pad, y, colW, 56);

        // Preview occupies the area to the right of the control column.
        const int previewX = pad + colW + 10;
        previewArea = juce::Rectangle<int>(previewX, 24,
                                           juce::jmax(80, getWidth() - previewX - pad),
                                           juce::jmax(80, getHeight() - 24 - pad));
    }

private:
    //==========================================================================
    void initLabel(juce::Label& lbl, const juce::String& text)
    {
        lbl.setText(text, juce::dontSendNotification);
        lbl.setJustificationType(juce::Justification::centredRight);
        lbl.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        addAndMakeVisible(lbl);
    }

    void initSlider(juce::Slider& s, double lo, double hi, double step, double val)
    {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, Sp3ctraTheme::kControlH);
        s.setRange(lo, hi, step);
        s.setValue(val, juce::dontSendNotification);
        addAndMakeVisible(s);
    }

    //==========================================================================
    void chooseWav()
    {
        const juce::File start = startDir();
        fileChooser = std::make_unique<juce::FileChooser>(
            "Select an audio file", start, "*.wav;*.aif;*.aiff;*.flac");
        fileChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safe = juce::Component::SafePointer<ScoreGenTabComponent>(this)]
            (const juce::FileChooser& fc)
            {
                auto* self = safe.getComponent();
                if (self == nullptr) return;
                const auto f = fc.getResult();
                if (f.existsAsFile())
                    self->setLoadedFile(f);
            });
    }

    void setLoadedFile(const juce::File& f)
    {
        loadedWav = f;
        const auto info = scoregen::probeWav(f);
        if (info.ok)
        {
            fileLabel.setText(f.getFileName() + "  ("
                              + juce::String(info.durationSec, 2) + "s, "
                              + juce::String(info.sampleRate) + " Hz, "
                              + juce::String(info.numChannels) + " ch)",
                              juce::dontSendNotification);
        }
        else
        {
            loadedWav = juce::File();
            fileLabel.setText("Unreadable: " + info.error, juce::dontSendNotification);
        }
    }

    void startGenerate()
    {
        if (! loadedWav.existsAsFile())
        {
            fileLabel.setText("Load a WAV first", juce::dontSendNotification);
            return;
        }
        // Generation parameters come from the shared SETUP settings; the
        // frequency span follows the instrument's musical Tuning + Root Note +
        // Octaves (like LuxStral), overriding any stored min/max.
        ScoreSettings s = processor.getScoreSettings();
        double lo = 0.0, hi = 0.0;
        processor.getScoreFrequencyRange(lo, hi);
        s.minFreq = lo;
        s.maxFreq = hi;
        genMinFreq = lo;   // remembered so playback can map the band's linear
        genMaxFreq = hi;   // freq axis onto the synth's log pixel distribution
        busy = true;
        progress = 0.0;
        generateButton.setEnabled(false);
        exportPngButton.setEnabled(false);
        exportJpgButton.setEnabled(false);
        if (auto* fs = processor.getLuxSampler())
            fs->uiStopScore();
        playStopButton.setEnabled(false);
        loopToggle.setEnabled(false);
        speedSlider.setEnabled(false);
        refreshPlayButton();
        progressBar.setVisible(true);
        previewImage = juce::Image();
        logLabel.setText("Generating...", juce::dontSendNotification);
        repaint();
        job.start(loadedWav, s);
    }

    void onRenderFinished(scoregen::RenderResult r)
    {
        busy = false;
        progressBar.setVisible(false);
        generateButton.setEnabled(true);

        if (r.ok && r.image.isValid())
        {
            generatedImage = r.image;
            spectroBand    = r.spectroBand;
            buildPreview();
            exportPngButton.setEnabled(true);
            exportJpgButton.setEnabled(true);
            logLabel.setText(r.log + "\n" + previewStats, juce::dontSendNotification);

            // Make the image playable through the LuxSampler engine (CHAIN 1).
            // Only the spectrogram band is read (matches the CIS sensor span);
            // the band's linear freq axis is remapped onto the synth's log axis.
            if (auto* fs = processor.getLuxSampler())
                fs->loadScoreFramesFromImage(generatedImage, spectroBand,
                                             genMinFreq, genMaxFreq);
            playStopButton.setEnabled(true);
            loopToggle.setEnabled(true);
            speedSlider.setEnabled(true);
            refreshPlayButton();
        }
        else
        {
            generatedImage = juce::Image();
            previewImage   = juce::Image();
            logLabel.setText("Failed: " + r.log, juce::dontSendNotification);
            if (auto* fs = processor.getLuxSampler())
                fs->uiStopScore();
            playStopButton.setEnabled(false);
            loopToggle.setEnabled(false);
            speedSlider.setEnabled(false);
            refreshPlayButton();
        }
        repaint();
    }

    void buildPreview()
    {
        if (! generatedImage.isValid() || previewArea.isEmpty())
        {
            previewImage = generatedImage;
            return;
        }
        // Preview only the spectrogram band (what is actually played) — the
        // surrounding white page margins would just wash the thumbnail grey.
        juce::Rectangle<int> band =
            (spectroBand.getWidth() > 0 && spectroBand.getHeight() > 0)
                ? spectroBand.getIntersection(generatedImage.getBounds())
                : generatedImage.getBounds();
        if (band.isEmpty())
            band = generatedImage.getBounds();

        const int targetW = juce::jmax(80, previewArea.getWidth() - 4);
        const int targetH = juce::jmax(80, previewArea.getHeight() - 4);
        const double s = juce::jmin(1.0,
            juce::jmin((double) targetW / band.getWidth(),
                       (double) targetH / band.getHeight()));
        const int pw = juce::jmax(1, (int) (band.getWidth()  * s));
        const int ph = juce::jmax(1, (int) (band.getHeight() * s));

        // Faithful downscale of the band (same content as the export).
        juce::Image tmp = generatedImage.getClippedImage(band)
                              .rescaled(pw, ph, juce::Graphics::highResamplingQuality);

        // ── Display-only white-point lift ─────────────────────────────────────
        // The score uses a 60 dB LOG dynamic range (SCORE_USE_LOG_AMPLITUDE), so
        // the band legitimately carries a broad grey floor (faint energy). The
        // full-page export hides it behind big white margins; cropped to the band
        // it reads grey. We lift the typical floor toward white for the PREVIEW
        // ONLY — playback and export are untouched. The measured stats below make
        // the cause visible in the log.
        {
            juce::Image::BitmapData bd(tmp, juce::Image::BitmapData::readWrite);
            juce::int64 sum = 0;
            int mn = 255, mx = 0;
            const int total = pw * ph;
            for (int y = 0; y < ph; ++y)
                for (int x = 0; x < pw; ++x)
                {
                    const int v = bd.getPixelPointer(x, y)[0];
                    sum += v; mn = juce::jmin(mn, v); mx = juce::jmax(mx, v);
                }
            const double meanN = total > 0 ? (double) sum / total / 255.0 : 1.0;
            const double wp = juce::jlimit(0.45, 0.98, meanN);   // white point
            for (int y = 0; y < ph; ++y)
                for (int x = 0; x < pw; ++x)
                {
                    double n = bd.getPixelPointer(x, y)[0] / 255.0;
                    n = juce::jlimit(0.0, 1.0, n / wp);           // floor → white
                    const auto v = (juce::uint8) (n * 255.0 + 0.5);
                    bd.setPixelColour(x, y, juce::Colour(v, v, v));
                }
            previewStats = "Band grey: min=" + juce::String(mn)
                         + " mean=" + juce::String((int) (meanN * 255.0))
                         + " max=" + juce::String(mx)
                         + "  (preview white-point=" + juce::String(wp, 2) + ")";
        }
        previewImage = tmp;
    }

    void chooseExport(bool asPng)
    {
        if (! generatedImage.isValid())
            return;
        const juce::String ext = asPng ? "png" : "jpg";
        const juce::File suggested = startDir()
            .getChildFile(loadedWav.getFileNameWithoutExtension() + "_score." + ext);
        fileChooser = std::make_unique<juce::FileChooser>(
            "Export Score Image", suggested, "*." + ext);
        fileChooser->launchAsync(
            juce::FileBrowserComponent::saveMode
                | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [safe = juce::Component::SafePointer<ScoreGenTabComponent>(this), asPng, ext]
            (const juce::FileChooser& fc)
            {
                auto* self = safe.getComponent();
                if (self == nullptr) return;
                auto dest = fc.getResult();
                if (dest.getFullPathName().isEmpty()) return;
                dest = dest.withFileExtension(ext);
                const bool ok = scoregen::exportImage(self->generatedImage, dest, asPng);
                self->logLabel.setText(ok ? ("Exported: " + dest.getFileName())
                                          : "Export failed",
                                       juce::dontSendNotification);
            });
    }

    //==========================================================================
    void togglePlay()
    {
        auto* fs = processor.getLuxSampler();
        if (fs == nullptr) return;

        if (fs->isScorePlaying())
        {
            fs->uiStopScore();
        }
        else
        {
            // Push current transport settings before starting.
            fs->setScoreSpeed((float) speedSlider.getValue());
            fs->setScoreLoopMode(loopToggle.getToggleState() ? LoopMode::LOOP
                                                             : LoopMode::NONE);
            fs->uiPlayScore();
        }
        refreshPlayButton();
    }

    void refreshPlayButton()
    {
        auto* fs = processor.getLuxSampler();
        const bool playing = (fs != nullptr) && fs->isScorePlaying();
        playStopButton.setButtonText(playing ? "STOP" : "PLAY");
    }

    void timerCallback() override
    {
        // Keep the button in sync when LoopMode::NONE playback ends on its own.
        refreshPlayButton();
        // Animate the reading head while playing.
        auto* fs = processor.getLuxSampler();
        if (fs != nullptr && fs->isScorePlaying())
            repaint(previewArea);
    }

    juce::File startDir() const
    {
        const auto out = processor.getSamplerOutputDir();
        if (out.isNotEmpty() && juce::File(out).isDirectory())
            return juce::File(out);
        if (loadedWav.existsAsFile())
            return loadedWav.getParentDirectory();
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    }

    //==========================================================================
    Sp3ctraAudioProcessor& processor;

    juce::TextButton loadButton, generateButton, exportPngButton, exportJpgButton;
    juce::Label      fileLabel, logLabel;

    // Writing Speed — essential generation control kept on the PLAY page.
    juce::Label  wsLabel;
    juce::Slider wsSlider;

    // Playback transport (CHAIN 1 — reuses the LuxSampler engine).
    juce::TextButton   playStopButton;
    juce::ToggleButton loopToggle;
    juce::Slider       speedSlider;
    juce::Label        speedLabel, playHint;

    double progress { 0.0 };
    juce::ProgressBar progressBar { progress };

    juce::Rectangle<int> previewArea;
    juce::Rectangle<int> spectroBand;  // band region inside generatedImage (played part)
    double genMinFreq { 0.0 };         // freq bounds used at GENERATE (Hz)
    double genMaxFreq { 0.0 };
    juce::String previewStats;         // band grey diagnostics (shown in log)
    juce::Image generatedImage;   // full resolution (export source)
    juce::Image previewImage;     // downscaled for painting (band crop)
    bool busy { false };

    juce::File loadedWav;
    std::unique_ptr<juce::FileChooser> fileChooser;

    ScoreGenJob job;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScoreGenTabComponent)
};
