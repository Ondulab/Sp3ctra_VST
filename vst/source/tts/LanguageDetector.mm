/**
 * @file LanguageDetector.mm
 * @brief NaturalLanguage implementation of LanguageDetector (macOS).
 */
#include "LanguageDetector.h"

#import <NaturalLanguage/NaturalLanguage.h>

juce::String LanguageDetector::detect (const juce::String& text,
                                       const juce::StringArray& allowedIsoCodes)
{
    if (text.trim().length() < 8)
        return {};

    @autoreleasepool
    {
        NLLanguageRecognizer* recognizer = [[[NLLanguageRecognizer alloc] init] autorelease];

        if (! allowedIsoCodes.isEmpty())
        {
            // NLLanguage values are BCP-47 strings ("fr", "en", …) — the ISO
            // codes parsed from the installed voice bundles map directly.
            NSMutableArray<NLLanguage>* langs =
                [NSMutableArray arrayWithCapacity: (NSUInteger) allowedIsoCodes.size()];
            for (const auto& code : allowedIsoCodes)
                [langs addObject: [NSString stringWithUTF8String: code.toRawUTF8()]];
            recognizer.languageConstraints = langs;
        }

        [recognizer processString: [NSString stringWithUTF8String: text.toRawUTF8()]];
        NLLanguage dominant = recognizer.dominantLanguage;

        return dominant != nil ? juce::String::fromUTF8 ([dominant UTF8String])
                               : juce::String();
    }
}
