/**
 * @file LanguageDetector.h
 * @brief VOICE module — dominant-language detection for the AUTO voice mode
 *        (macOS NaturalLanguage / NLLanguageRecognizer behind a pImpl-free
 *        static facade; implementation in LanguageDetector.mm).
 *
 * Any thread; stateless and cheap (one recognizer per call).
 */
#pragma once

#include <juce_core/juce_core.h>

namespace LanguageDetector
{
    /** Returns the dominant ISO 639-1 code ("fr", "en", …) of the text, or an
     *  empty string when undecided. Constraining to the languages that
     *  actually have installed voices (allowedIsoCodes) makes short sentences
     *  reliable; pass an empty array for unconstrained detection. Texts
     *  shorter than 8 characters return empty (too little signal). */
    juce::String detect (const juce::String& text,
                         const juce::StringArray& allowedIsoCodes);
}
