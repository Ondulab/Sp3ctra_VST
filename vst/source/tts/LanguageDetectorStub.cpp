/**
 * @file LanguageDetectorStub.cpp
 * @brief Non-macOS stand-in for the NaturalLanguage-based detector. Returns
 *        "undecided" for every text, which makes the VOICE module's AUTO mode
 *        fall back to its manual language selection path.
 */
#include "LanguageDetector.h"

namespace LanguageDetector
{
    juce::String detect (const juce::String& text,
                         const juce::StringArray& allowedIsoCodes)
    {
        juce::ignoreUnused (text, allowedIsoCodes);
        return {};
    }
}
