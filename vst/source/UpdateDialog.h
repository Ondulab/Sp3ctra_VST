/**
 * @file UpdateDialog.h
 * @brief "Software Update" overlay — drives AppUpdater: check on open, then
 *        INSTALL NOW → download progress → RESTART NOW.
 *
 * Same overlay idiom as AboutDialog (parented to the top-level component,
 * centred, header-only) but closes only via its CLOSE button: a stray click
 * must not hide a running download. Closing never cancels anything — the
 * AppUpdater singleton keeps the state and the ABOUT badge stays lit.
 * Builds that cannot self-install (VST3/AU in a DAW, translocated or
 * read-only installs) get the downloads-page link instead of INSTALL.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "AppUpdater.h"
#include "OndulabLinks.h"
#include "UITheme.h"
#include "Sp3ctraVersion.h"

class UpdateDialog : public juce::Component,
                     private juce::ChangeListener,
                     private juce::Timer
{
public:
    static void show(juce::Component* parent)
    {
        if (parent == nullptr) return;
        juce::Component* host = parent->getTopLevelComponent();
        if (host == nullptr) host = parent;

        // The updater is a singleton — never stack a second view of it.
        for (auto* child : host->getChildren())
            if (auto* existing = dynamic_cast<UpdateDialog*>(child))
            {
                existing->toFront(true);
                return;
            }

        auto* dlg = new UpdateDialog();
        dlg->setSize(440, 240);
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
        g.drawText("Software Update", 24, 18, getWidth() - 48, 22,
                   juce::Justification::centredLeft);

        g.setColour(juce::Colour(0xff8a93a5));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        g.drawText("Installed: v" SP3CTRA_VERSION_STRING "  (" SP3CTRA_BUILD_DATE ")",
                   24, 46, getWidth() - 48, 16, juce::Justification::centredLeft);

        auto* up = AppUpdater::getInstance();
        const auto st = up->state();

        juce::String status;
        juce::Colour statusCol(0xffdde3e8);
        switch (st)
        {
            case AppUpdater::State::idle:
            case AppUpdater::State::checking:
                status = juce::String::fromUTF8("Checking for updates…"); break;
            case AppUpdater::State::upToDate:
                status = "You are up to date."; break;
            case AppUpdater::State::updateAvailable:
                status = "New version available: v" + up->latestVersion();
                if (! AppUpdater::canSelfInstall())
                    status += juce::String::fromUTF8(
                        "\nThis build cannot update itself — "
                        "please use the downloads page.");
                break;
            case AppUpdater::State::downloading:
                status = juce::String::fromUTF8("Downloading v") + up->latestVersion()
                       + juce::String::fromUTF8("…   ") + up->progressText();
                break;
            case AppUpdater::State::installing:
                status = juce::String::fromUTF8("Installing…"); break;
            case AppUpdater::State::readyToRestart:
                status = "Update v" + up->latestVersion()
                       + " is ready — restart Sp3ctra to run it.";
                break;
            case AppUpdater::State::failed:
                status = up->errorMessage();
                statusCol = juce::Colour(0xffd07070);
                break;
        }
        g.setColour(statusCol);
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        g.drawFittedText(status, 24, 78, getWidth() - 48, 40,
                         juce::Justification::topLeft, 2);

        if (st == AppUpdater::State::downloading)
        {
            const juce::Rectangle<float> track(24.f, 132.f, (float) getWidth() - 48.f, 8.f);
            g.setColour(juce::Colour(0xff2a2a2a));
            g.fillRoundedRectangle(track, 4.f);
            const float p = up->progress();
            if (p > 0.f)
            {
                g.setColour(juce::Colour(0xff4fa3e0));
                g.fillRoundedRectangle(track.withWidth(track.getWidth()
                                                       * juce::jlimit(0.f, 1.f, p)), 4.f);
            }
        }
    }

    void resized() override
    {
        actionBtn_.setBounds(24, getHeight() - 42, 170, 30);
        downloadsLink_.setBounds(24, getHeight() - 42, 200, 30);
        closeBtn_.setBounds(getWidth() - 96 - 16, getHeight() - 42, 96, 30);
    }

private:
    UpdateDialog()
    {
        actionBtn_.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xff2a4a66));
        actionBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffd0e0f0));
        actionBtn_.onClick = [] {
            auto* up = AppUpdater::getInstance();
            switch (up->state())
            {
                case AppUpdater::State::updateAvailable: up->downloadAndInstall(); break;
                case AppUpdater::State::readyToRestart:  up->restartNow();         break;
                default:                                 up->check();              break;
            }
        };
        addAndMakeVisible(actionBtn_);

        downloadsLink_.setButtonText(juce::String::fromUTF8("Open the downloads page…"));
        downloadsLink_.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings), false,
                               juce::Justification::centredLeft);
        downloadsLink_.setColour(juce::HyperlinkButton::textColourId,
                                 juce::Colour(0xff88aaff));
        downloadsLink_.setURL(juce::URL(OndulabLinks::kDownloadsUrl));
        addChildComponent(downloadsLink_);

        closeBtn_.setButtonText("CLOSE");
        closeBtn_.setColour(juce::TextButton::buttonColourId,  juce::Colour(0xff242424));
        closeBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff999999));
        closeBtn_.onClick = [this] { dismiss(); };
        addAndMakeVisible(closeBtn_);

        auto* up = AppUpdater::getInstance();
        up->addChangeListener(this);
        const auto st = up->state();
        if (st == AppUpdater::State::idle || st == AppUpdater::State::upToDate
            || st == AppUpdater::State::failed)
            up->check();   // opening the dialog IS "check now"

        refresh();
        startTimerHz(10);  // live progress text/bar while downloading
    }

    ~UpdateDialog() override
    {
        if (auto* up = AppUpdater::getInstanceWithoutCreating())
            up->removeChangeListener(this);
    }

    void refresh()
    {
        auto* up = AppUpdater::getInstance();
        const auto st = up->state();
        const bool self = AppUpdater::canSelfInstall();

        juce::String label;
        bool enabled = true, showLink = false;
        switch (st)
        {
            case AppUpdater::State::idle:            label = "CHECK NOW";   break;
            case AppUpdater::State::checking:        label = juce::String::fromUTF8("CHECKING…");
                                                     enabled = false;       break;
            case AppUpdater::State::upToDate:        label = "CHECK AGAIN"; break;
            case AppUpdater::State::updateAvailable:
                if (self) label = "INSTALL NOW";
                else      showLink = true;
                break;
            case AppUpdater::State::downloading:     label = juce::String::fromUTF8("DOWNLOADING…");
                                                     enabled = false;       break;
            case AppUpdater::State::installing:      label = juce::String::fromUTF8("INSTALLING…");
                                                     enabled = false;       break;
            case AppUpdater::State::readyToRestart:  label = "RESTART NOW"; break;
            case AppUpdater::State::failed:          label = "RETRY";       break;
        }
        actionBtn_.setVisible(! showLink);
        actionBtn_.setEnabled(enabled);
        if (label.isNotEmpty())
            actionBtn_.setButtonText(label);
        downloadsLink_.setVisible(showLink);
        repaint();
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }

    void timerCallback() override
    {
        if (AppUpdater::getInstance()->state() == AppUpdater::State::downloading)
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
            [sp = juce::Component::SafePointer<UpdateDialog>(this)]
            {
                if (sp != nullptr)
                {
                    if (auto* p = sp->getParentComponent())
                        p->removeChildComponent(sp.getComponent());
                    delete sp.getComponent();
                }
            });
    }

    juce::TextButton actionBtn_;
    juce::HyperlinkButton downloadsLink_;
    juce::TextButton closeBtn_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UpdateDialog)
};
