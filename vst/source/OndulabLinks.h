/**
 * @file OndulabLinks.h
 * @brief The Ondulab web links, shared by AboutDialog, UpdateDialog and the
 *        ABOUT menu so they never diverge.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Sp3ctraVersion.h"

namespace OndulabLinks
{
constexpr const char* kWebsiteUrl   = "https://www.ondulab.com";
constexpr const char* kDownloadsUrl = "https://www.ondulab.com/telechargement.html";
constexpr const char* kBuyUrl       = "https://www.ondulab.com/acheter.html";
constexpr const char* kDonateUrl    = "https://paypal.me/Ondulab";
constexpr const char* kLicenseUrl   = "https://www.gnu.org/licenses/gpl-3.0.html";
constexpr const char* kSourceUrl    = "https://github.com/Ondulab/Sp3ctra_VST";
constexpr const char* kReleasesUrl  = "https://github.com/Ondulab/Sp3ctra_VST/releases";
constexpr const char* kIssuesUrl    = "https://github.com/Ondulab/Sp3ctra_VST/issues";
constexpr const char* kNoticesUrl   =
    "https://github.com/Ondulab/Sp3ctra_VST/blob/master/THIRD-PARTY-NOTICES.md";

inline juce::String contactUrl()
{
    // Version in the subject so support emails identify the build at a glance.
    return juce::String ("mailto:contact@ondulab.com?subject=")
         + juce::URL::addEscapeChars ("Sp3ctra v" SP3CTRA_VERSION_STRING, true);
}
} // namespace OndulabLinks
