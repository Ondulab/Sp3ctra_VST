/**
 * @file AboutDialog.h
 * @brief "About Sp3ctra" overlay — logo, tagline, version, license, credits,
 *        the project links (source / releases / issues on GitHub), the entry
 *        to the in-app software update (UpdateDialog) and to the credits
 *        overlay (CreditsDialog).
 *
 * Same overlay idiom as Sp3ctraDialog: parented to the top-level component,
 * centred, self-deleting on close. Header-only, no separate TU.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "UITheme.h"
#include "IconPaths.h"
#include "Sp3ctraVersion.h"
#include "OndulabLinks.h"
#include "UpdateDialog.h"
#include "CreditsDialog.h"

class AboutDialog : public juce::Component
{
public:
    static void show(juce::Component* parent)
    {
        if (parent == nullptr) return;
        juce::Component* host = parent->getTopLevelComponent();
        if (host == nullptr) host = parent;

        auto* dlg = new AboutDialog();
        dlg->setSize(420, 340);
        host->addAndMakeVisible(dlg);
        dlg->centreOnParent();
        dlg->toFront(true);
    }

    void paint(juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xf21e1e1e));
        g.fillRoundedRectangle(b, 6.0f);
        g.setColour(juce::Colour(0xff444444));
        g.drawRoundedRectangle(b.reduced(0.5f), 6.0f, 1.0f);

        // ── Logo + name ──────────────────────────────────────────────────────
        Icons::drawSp3ctraLogoPicto(g, { 24.f, 22.f, 30.f, 34.f });
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(juce::FontOptions(22.f)).boldened());
        g.drawText("Sp3ctra", 64, 20, getWidth() - 88, 24,
                   juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xff8a93a5));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        g.drawText(juce::String::fromUTF8("Draw the sound, play the images."),
                   64, 44, getWidth() - 88, 16, juce::Justification::centredLeft);

        // ── Version + license + credits ──────────────────────────────────────
        g.setColour(juce::Colour(0xffdde3e8));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        g.drawText("Version " SP3CTRA_VERSION_STRING,
                   24, 76, getWidth() - 48, 18, juce::Justification::centredLeft);

        g.setColour(juce::Colour(0xff8a9aaa));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        g.drawFittedText(
            juce::String::fromUTF8(
                "© Ondulab — free software under the GNU GPL v3 (or later) license.\n"
                "Made with ♥ in Marseille.\n"
                "Thanks to Réso-Nance Numérique, Universcience, Devisubox —\n"
                "and to everyone drawing sound with Sp3ctra."),
            24, 98, getWidth() - 48, 70, juce::Justification::topLeft, 5);
    }

    void resized() override
    {
        // Link rows (left) + CLOSE button (bottom right).
        const int x = 24, w = getWidth() - 48, rowH = 20;
        int y = 174;
        for (auto* l : links_)
        {
            l->setBounds(x, y, w, rowH);
            y += rowH + 2;
        }
        closeBtn_.setBounds(getWidth() - 96 - 16, getHeight() - 36 - 12, 96, 30);
    }

    /** Click anywhere outside the links/button also closes (light-weight). */
    void mouseUp(const juce::MouseEvent&) override { dismiss(); }

private:
    AboutDialog()
    {
        auto addLink = [this](const juce::String& text, const juce::String& url)
        {
            auto* l = links_.add(new juce::HyperlinkButton(text, juce::URL(url)));
            l->setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings), false,
                       juce::Justification::centredLeft);
            l->setColour(juce::HyperlinkButton::textColourId,
                         juce::Colour(0xff88aaff));
            addAndMakeVisible(l);
        };
        // Local-action rows (update / credits overlays): empty URL is not
        // well-formed so HyperlinkButton::clicked() launches nothing and only
        // onClick runs.
        auto addAction = [this](const juce::String& text,
                                std::function<void(juce::Component*)> action)
        {
            auto* l = links_.add(new juce::HyperlinkButton(text, juce::URL()));
            l->setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings), false,
                       juce::Justification::centredLeft);
            l->setColour(juce::HyperlinkButton::textColourId,
                         juce::Colour(0xff88aaff));
            l->onClick = [this, action]
            {
                auto* host = getParentComponent();
                dismiss();
                action(host);
            };
            addAndMakeVisible(l);
        };

        addAction(juce::String::fromUTF8("Software update…"),
                  [](juce::Component* host) { UpdateDialog::show(host); });
        addLink(juce::String::fromUTF8("Source code — GitHub"),
                OndulabLinks::kSourceUrl);
        addLink(juce::String::fromUTF8("Release notes"),
                OndulabLinks::kReleasesUrl);
        addLink(juce::String::fromUTF8("Report an issue — GitHub"),
                OndulabLinks::kIssuesUrl);
        addAction(juce::String::fromUTF8("Credits & licenses"),
                  [](juce::Component* host) { CreditsDialog::show(host); });

        closeBtn_.setButtonText("CLOSE");
        closeBtn_.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xff242424));
        closeBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff999999));
        closeBtn_.onClick = [this] { dismiss(); };
        addAndMakeVisible(closeBtn_);
    }

    void centreOnParent()
    {
        if (auto* p = getParentComponent())
        {
            auto r = getBounds().withCentre(p->getLocalBounds().getCentre());
            r.setPosition(juce::jmax(0, r.getX()), juce::jmax(0, r.getY()));
            setBounds(r);
        }
    }
    void parentSizeChanged() override { centreOnParent(); }

    void dismiss()
    {
        juce::MessageManager::callAsync(
            [sp = juce::Component::SafePointer<AboutDialog>(this)]
            {
                if (sp != nullptr)
                {
                    if (auto* p = sp->getParentComponent())
                        p->removeChildComponent(sp.getComponent());
                    delete sp.getComponent();
                }
            });
    }

    juce::OwnedArray<juce::HyperlinkButton> links_;
    juce::TextButton closeBtn_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AboutDialog)
};
