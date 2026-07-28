/**
 * @file CreditsDialog.h
 * @brief "Credits & licenses" overlay — condensed view of
 *        THIRD-PARTY-NOTICES.md (statically linked components and embedded
 *        voices) plus links to the full notices and the GPL v3 text.
 *
 * Same overlay idiom as AboutDialog: parented to the top-level component,
 * centred, self-deleting on close. Header-only, no separate TU.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "UITheme.h"
#include "OndulabLinks.h"

class CreditsDialog : public juce::Component
{
public:
    static void show(juce::Component* parent)
    {
        if (parent == nullptr) return;
        juce::Component* host = parent->getTopLevelComponent();
        if (host == nullptr) host = parent;

        auto* dlg = new CreditsDialog();
        dlg->setSize(460, 360);
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

        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(juce::FontOptions(18.f)).boldened());
        g.drawText(juce::String::fromUTF8("Credits & licenses"),
                   24, 20, getWidth() - 48, 22, juce::Justification::centredLeft);

        g.setColour(juce::Colour(0xff8a9aaa));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        g.drawFittedText(
            juce::String::fromUTF8(
                "Sp3ctra is a combined work distributed under the GNU GPL v3\n"
                "(or later). It statically links:\n"
                "\n"
                "JUCE 8 — AGPL v3 · VST3 SDK — GPL v3\n"
                "AudioUnit SDK — Apache 2.0\n"
                "sherpa-onnx — Apache 2.0 · onnxruntime — MIT\n"
                "espeak-ng — GPL v3+ · piper-phonemize — MIT\n"
                "KissFFT — BSD 3-Clause\n"
                "\n"
                "Embedded Piper voices: SIWIS fr-FR (CC-BY 4.0),\n"
                "LJ Speech en-US (public domain)."),
            24, 54, getWidth() - 48, 216, juce::Justification::topLeft, 14);
    }

    void resized() override
    {
        const int x = 24, w = getWidth() - 48, rowH = 20;
        int y = getHeight() - 118;
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
    CreditsDialog()
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
        addLink(juce::String::fromUTF8("Full notices — THIRD-PARTY-NOTICES.md"),
                OndulabLinks::kNoticesUrl);
        addLink(juce::String::fromUTF8("GNU GPL v3 License"),
                OndulabLinks::kLicenseUrl);

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
            [sp = juce::Component::SafePointer<CreditsDialog>(this)]
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CreditsDialog)
};
