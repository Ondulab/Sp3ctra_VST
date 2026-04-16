#include "BlobVisualizerComponent.h"
#include "../PluginProcessor.h"
#include "../Sp3ctraCore.h"

extern "C" {
    #include "../config/config_loader.h"
    #include "../config/config_instrument.h"
}

// Colours
static const juce::Colour kBgColour     { 0xff0d0d0d };
static const juce::Colour kAboveColour  { 0xff00c8aa };   // teal — pixel above threshold
static const juce::Colour kBlobRect     { 0xffff8800 };   // orange bounding box
static const juce::Colour kBadgeBg      { 0xff1c3755 };

//==============================================================================
BlobVisualizerComponent::BlobVisualizerComponent(Sp3ctraAudioProcessor& p)
    : processor(p)
{
    startTimerHz(30);
}

BlobVisualizerComponent::~BlobVisualizerComponent()
{
    stopTimer();
}

//==============================================================================
void BlobVisualizerComponent::timerCallback()
{
    rebuildImage();
    repaint();
}

//==============================================================================
void BlobVisualizerComponent::rebuildImage()
{
    const int numPixels = get_cis_pixels_nb();
    if (numPixels <= 0) return;

    // Read config snapshot (non-RT)
    extern sp3ctra_config_t g_sp3ctra_config;
    const float threshold = g_sp3ctra_config.strokeforge_blob_base_threshold;

    // ── Blob detection runs on the mix-final gray buffer. ─────────────────────
    // CisVisualizerComponent::updateCisData() publishes `finalGrayBuffer_` every
    // timer tick.  It already contains the complete image pipeline:
    //   RGB → gray, opacity blend, inversion, gamma, sampler-playback mix, and
    //   sequencer STEP_EMPTY white-out.
    // Reading from this shared buffer guarantees that blob detection is always
    // in sync with the image the user actually sees — including:
    //   - STOP (all-white → no blobs)
    //   - HOLD (frozen frame → blobs on the frozen image)
    //   - Sampler playback with full processing
    //   - Sequencer STEP_EMPTY (white → no blobs)
    // Previous approach (raw AudioImageBuffers + manual gamma) was both incorrect
    // (missed sampler processing) and redundant (duplicated the pipeline).
    auto* fs = processor.getLuxSampler();
    if (!fs) return;

    const auto& grayBuf = fs->getFinalGrayBuffer();
    if ((int)grayBuf.size() < numPixels)
    {
        // CisVisualizerComponent has not pushed data yet (startup grace period).
        detectedBlobs.clear();
        blobCount = 0;
        return;
    }

    // All image transforms (inversion, gamma) are already baked into grayBuf.
    // Normalise to [0..1] — no further pipeline step required.
    std::vector<float> line(numPixels);
    for (int i = 0; i < numPixels; ++i)
        line[i] = grayBuf[i] / 255.0f;

    detectBlobs(line.data(), numPixels);

    // Draw to image
    const int w = getWidth();
    const int h = getHeight();
    if (w <= 0 || h <= 0) return;

    if (blobImage.getWidth() != w || blobImage.getHeight() != h)
        blobImage = juce::Image(juce::Image::RGB, w, h, false);

    juce::Graphics ig(blobImage);
    ig.fillAll(kBgColour);

    // Render thresholded strip (1-pixel-high mapped to full width → column rendering)
    const float scaleX = (float)w / (float)numPixels;
    for (int i = 0; i < numPixels; ++i)
    {
        const float val = line[i];
        if (val >= threshold)
        {
            // Intensity-weighted teal column
            const float brightness = juce::jlimit(0.0f, 1.0f, (val - threshold) / (1.0f - threshold + 1e-6f));
            ig.setColour(kAboveColour.withAlpha(0.4f + 0.6f * brightness));
            const int x0 = juce::roundToInt(i * scaleX);
            const int x1 = juce::roundToInt((i + 1) * scaleX);
            ig.fillRect(x0, 0, juce::jmax(1, x1 - x0), h);
        }
    }

    // Blob bounding boxes
    ig.setColour(kBlobRect);
    for (auto& blob : detectedBlobs)
    {
        const int x0 = juce::roundToInt(blob.startPx * scaleX);
        const int x1 = juce::roundToInt(blob.endPx   * scaleX);
        ig.drawRect(x0, 1, juce::jmax(2, x1 - x0), h - 2, 1);
    }
}

//==============================================================================
void BlobVisualizerComponent::detectBlobs(const float* line, int numPixels)
{
    extern sp3ctra_config_t g_sp3ctra_config;
    const float threshold = g_sp3ctra_config.strokeforge_blob_base_threshold;
    const int   minWidth  = g_sp3ctra_config.strokeforge_blob_min_width;
    const int   mergeGap  = g_sp3ctra_config.strokeforge_blob_merge_gap;

    detectedBlobs.clear();

    // Simple 1D connected-component scan
    bool inBlob = false;
    int  blobStart = 0;
    int  gapCount  = 0;

    for (int i = 0; i < numPixels; ++i)
    {
        const bool above = (line[i] >= threshold);

        if (above)
        {
            if (!inBlob)
            {
                blobStart = i;
                inBlob = true;
            }
            gapCount = 0;
        }
        else
        {
            if (inBlob)
            {
                ++gapCount;
                if (gapCount > mergeGap)
                {
                    const int width = (i - gapCount) - blobStart;
                    if (width >= minWidth)
                        detectedBlobs.push_back({ blobStart, i - gapCount });
                    inBlob    = false;
                    gapCount  = 0;
                }
            }
        }
    }
    if (inBlob)
    {
        const int width = numPixels - blobStart;
        if (width >= minWidth)
            detectedBlobs.push_back({ blobStart, numPixels });
    }

    blobCount = (int)detectedBlobs.size();
}

//==============================================================================
void BlobVisualizerComponent::paint(juce::Graphics& g)
{
    if (blobImage.isValid() && blobImage.getWidth() == getWidth())
        g.drawImageAt(blobImage, 0, 0);
    else
        g.fillAll(kBgColour);

    // Blob count badge (top-right corner)
    const juce::String label = juce::String(blobCount) + " blob" + (blobCount != 1 ? "s" : "");
    const int bw = 80, bh = 16;
    g.setColour(kBadgeBg);
    g.fillRoundedRectangle(juce::Rectangle<int>(getWidth() - bw - 4, 3, bw, bh).toFloat(), 3.f);
    g.setColour(juce::Colours::white.withAlpha(0.85f));
    g.setFont(juce::Font(juce::FontOptions(10.f)));
    g.drawText(label, getWidth() - bw - 4, 3, bw, bh, juce::Justification::centred);

    // Border
    g.setColour(juce::Colour(0xff333333));
    g.drawRect(getLocalBounds(), 1);
}

void BlobVisualizerComponent::resized() {}
