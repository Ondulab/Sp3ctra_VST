#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

class Sp3ctraAudioProcessor;

/**
 * @brief Second visualizer — shows the CIS image thresholded for blob detection.
 *
 * Renders:
 *  - Pixels above sfBlobBaseThreshold (after inversion+gamma) in teal
 *  - Pixels below threshold in dark
 *  - Detected blob bounding boxes as orange rectangles
 *  - Blob count badge (top-right)
 *
 * Architecture: reads raw AudioImageBuffers on the UI timer thread (read-only,
 * lock-free). Blob detection is done independently here (UI copy, not RT-coupled).
 */
class BlobVisualizerComponent : public juce::Component,
                                 public juce::Timer
{
public:
    explicit BlobVisualizerComponent(Sp3ctraAudioProcessor& p);
    ~BlobVisualizerComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override;

private:
    struct BlobRect { int startPx; int endPx; };

    Sp3ctraAudioProcessor& processor;

    // Rendered image (built on timer thread, painted on message thread)
    juce::Image blobImage { juce::Image::RGB, 1, 1, false };
    std::vector<BlobRect> detectedBlobs;
    int blobCount = 0;

    void rebuildImage();
    void detectBlobs(const float* thresholdedLine, int numPixels);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BlobVisualizerComponent)
};
