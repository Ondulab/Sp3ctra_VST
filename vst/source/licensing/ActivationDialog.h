/**
 * @file ActivationDialog.h
 * @brief License overlay — demo explanation, buy link, key entry/activation,
 *        and the licensed view (masked key + deactivate).
 *
 * Same overlay idiom as AboutDialog: parented to the top-level component,
 * centred, self-deleting on close. Header-only, no separate TU.
 *
 * Also hosts LicenseGate::blockIfDemo(), the one-liner every save/export
 * flow calls first — it opens this dialog with the blocked action named.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../UITheme.h"
#include "../IconPaths.h"
#include "../OndulabLinks.h"
#include "../Sp3ctraDialog.h"
#include "LicenseManager.h"

class ActivationDialog : public juce::Component,
                         private juce::ChangeListener
{
public:
    /** @p blockedAction names the save/export the user just tried in demo
     *  mode ("Export image", …); empty when opened from the ABOUT menu. */
    static void show(juce::Component* parent,
                     const juce::String& blockedAction = {})
    {
        if (parent == nullptr) return;
        juce::Component* host = parent->getTopLevelComponent();
        if (host == nullptr) host = parent;

        // One at a time — re-invoking just refreshes the blocked-action line.
        for (auto* c : host->getChildren())
            if (auto* existing = dynamic_cast<ActivationDialog*>(c))
            {
                existing->blockedAction_ = blockedAction;
                existing->toFront(true);
                existing->repaint();
                return;
            }

        auto* dlg = new ActivationDialog(blockedAction);
        dlg->setSize(440, 330);
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

        const bool lic = LicenseManager::isLicensed();

        // ── Logo + title ─────────────────────────────────────────────────────
        Icons::drawSp3ctraLogoPicto(g, { 24.f, 22.f, 30.f, 34.f });
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(juce::FontOptions(22.f)).boldened());
        g.drawText("Sp3ctra", 64, 20, getWidth() - 88, 24,
                   juce::Justification::centredLeft);
        g.setColour(lic ? juce::Colour(0xff7fd88f) : juce::Colour(0xffe0b060));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        g.drawText(lic ? "Studio mode" : "Play mode",
                   64, 44, getWidth() - 88, 16, juce::Justification::centredLeft);

        // ── Body ─────────────────────────────────────────────────────────────
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        int y = 76;

        if (! lic && blockedAction_.isNotEmpty())
        {
            g.setColour(juce::Colour(0xffe0b060));
            g.drawFittedText(juce::String::fromUTF8("“") + blockedAction_
                                 + juce::String::fromUTF8("” is available in "
                                                          "Studio mode."),
                             24, y, getWidth() - 48, 18,
                             juce::Justification::centredLeft, 1);
            y += 24;
        }

        g.setColour(juce::Colour(0xff8a9aaa));
        if (lic)
        {
            auto* lm = LicenseManager::getInstance();
            juce::String who = lm->licensedTo();
            g.drawFittedText(
                (who.isNotEmpty() ? "Licensed to " + who + "\n" : juce::String())
                    + "Key " + lm->maskedKey()
                    + juce::String::fromUTF8(
                        "\nThis machine is activated — Standalone, VST3 and AU."
                        "\nThank you for supporting Sp3ctra ♥"),
                24, y, getWidth() - 48, 76, juce::Justification::topLeft, 5);
        }
        else
        {
            g.drawFittedText(
                juce::String::fromUTF8(
                    "Play mode is the full instrument, without time limit.\n"
                    "Saving sessions and exporting (audio, video, images, "
                    "MIDI, presets) belong to Studio mode — and a DAW "
                    "project will not keep the plugin's state.\n"
                    "One license activates Studio mode on up to 3 machines."),
                24, y, getWidth() - 48, 76, juce::Justification::topLeft, 6);
        }
    }

    void resized() override
    {
        const int x = 24, w = getWidth() - 48;
        buyLink_.setBounds(x, 178, w, 20);
        keyEdit_.setBounds(x, 204, w - 110, 28);
        activateBtn_.setBounds(x + w - 100, 204, 100, 28);
        deactivateBtn_.setBounds(x, 204, 130, 28);
        status_.setBounds(x, 240, w, 34);
        closeBtn_.setBounds(getWidth() - 96 - 16, getHeight() - 30 - 12, 96, 30);
    }

private:
    explicit ActivationDialog(const juce::String& blockedAction)
        : blockedAction_(blockedAction)
    {
        buyLink_.setButtonText(juce::String::fromUTF8("Buy a license — ondulab.com"));
        buyLink_.setURL(juce::URL(OndulabLinks::kBuyUrl));
        buyLink_.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings), false,
                         juce::Justification::centredLeft);
        buyLink_.setColour(juce::HyperlinkButton::textColourId,
                           juce::Colour(0xff88aaff));
        addAndMakeVisible(buyLink_);

        keyEdit_.setTextToShowWhenEmpty("License key from your purchase email",
                                        juce::Colour(0xff667788));
        keyEdit_.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        keyEdit_.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff141414));
        keyEdit_.setColour(juce::TextEditor::outlineColourId,    juce::Colour(0xff444444));
        keyEdit_.setColour(juce::TextEditor::textColourId,       juce::Colours::white);
        keyEdit_.onReturnKey = [this] { activateNow(); };
        addAndMakeVisible(keyEdit_);

        activateBtn_.setButtonText("ACTIVATE");
        activateBtn_.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xff2a3a4a));
        activateBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff88ccff));
        activateBtn_.onClick = [this] { activateNow(); };
        addAndMakeVisible(activateBtn_);

        deactivateBtn_.setButtonText("DEACTIVATE");
        deactivateBtn_.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xff242424));
        deactivateBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff999999));
        deactivateBtn_.onClick = [this]
        {
            Sp3ctraDialog::showConfirm(
                this, "Deactivate this machine",
                "The license seat is freed for another machine and this "
                "install returns to Play mode (saving and exports disabled).",
                "Deactivate", "Cancel",
                [](bool ok) { if (ok) LicenseManager::getInstance()->deactivate(); });
        };
        addAndMakeVisible(deactivateBtn_);

        status_.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        status_.setJustificationType(juce::Justification::topLeft);
        status_.setMinimumHorizontalScale(1.0f);
        addAndMakeVisible(status_);

        closeBtn_.setButtonText("CLOSE");
        closeBtn_.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xff242424));
        closeBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff999999));
        closeBtn_.onClick = [this] { dismiss(); };
        addAndMakeVisible(closeBtn_);

        LicenseManager::getInstance()->addChangeListener(this);
        refresh();
    }

    ~ActivationDialog() override
    {
        if (auto* lm = LicenseManager::getInstanceWithoutCreating())
            lm->removeChangeListener(this);
    }

    void activateNow()
    {
        const auto key = keyEdit_.getText().trim();
        if (key.isEmpty())
        {
            status_.setColour(juce::Label::textColourId, juce::Colour(0xffe08080));
            status_.setText("Paste the license key from your purchase email.",
                            juce::dontSendNotification);
            return;
        }
        LicenseManager::getInstance()->activate(key);
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }

    void refresh()
    {
        auto* lm = LicenseManager::getInstance();
        const bool lic  = lm->licensed();
        const auto op   = lm->opState();
        const bool busy = op == LicenseManager::OpState::busy;

        buyLink_.setVisible(! lic);
        keyEdit_.setVisible(! lic);
        activateBtn_.setVisible(! lic);
        activateBtn_.setEnabled(! busy);
        keyEdit_.setEnabled(! busy);
        deactivateBtn_.setVisible(lic);
        deactivateBtn_.setEnabled(! busy);

        if (busy)
        {
            status_.setColour(juce::Label::textColourId, juce::Colour(0xff8a9aaa));
            status_.setText(juce::String::fromUTF8("Contacting the license server…"),
                            juce::dontSendNotification);
        }
        else if (op == LicenseManager::OpState::failed)
        {
            status_.setColour(juce::Label::textColourId, juce::Colour(0xffe08080));
            status_.setText(lm->lastError(), juce::dontSendNotification);
        }
        else if (lic)
        {
            status_.setColour(juce::Label::textColourId, juce::Colour(0xff7fd88f));
            status_.setText(juce::String::fromUTF8("Activated — this machine is "
                                                   "unlocked."),
                            juce::dontSendNotification);
        }
        else
            status_.setText({}, juce::dontSendNotification);

        repaint();
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
            [sp = juce::Component::SafePointer<ActivationDialog>(this)]
            {
                if (sp != nullptr)
                {
                    if (auto* p = sp->getParentComponent())
                        p->removeChildComponent(sp.getComponent());
                    delete sp.getComponent();
                }
            });
    }

    juce::String          blockedAction_;
    juce::HyperlinkButton buyLink_;
    juce::TextEditor      keyEdit_;
    juce::TextButton      activateBtn_, deactivateBtn_, closeBtn_;
    juce::Label           status_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ActivationDialog)
};

namespace LicenseGate
{
/** Call FIRST in every save/export flow: true = demo build, the action is
 *  blocked and the activation dialog (with @p action named) is up. */
inline bool blockIfDemo(juce::Component* anyComponentInTheUi,
                        const juce::String& action)
{
    if (LicenseManager::isLicensed())
        return false;
    ActivationDialog::show(anyComponentInTheUi, action);
    return true;
}
} // namespace LicenseGate
