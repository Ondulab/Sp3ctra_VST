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
constexpr const char* kDonateUrl    = "https://paypal.me/Ondulab";
constexpr const char* kLicenseUrl   = "https://www.gnu.org/licenses/gpl-3.0.html";

inline juce::String bugReportUrl()
{
    // Version in the subject so reports identify the build at a glance.
    return juce::String ("mailto:contact@ondulab.com?subject=")
         + juce::URL::addEscapeChars ("Sp3ctra v" SP3CTRA_VERSION_STRING
                                      " - bug report", true);
}
} // namespace OndulabLinks
