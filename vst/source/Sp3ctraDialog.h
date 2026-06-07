#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include "UITheme.h"

/**
 * @file Sp3ctraDialog.h
 * @brief Sp3ctra-themed modal dialogs (info / warning / confirm).
 *
 * These dialogs replace juce::AlertWindow::showMessageBoxAsync so that all
 * notifications follow the Sp3ctra dark visual identity:
 *   - Dark panel background, rounded corners
 *   - Hand-drawn warning triangle (no OS / teal "i" icon)
 *   - Compact buttons matching Sp3ctra control height
 *   - Consistent typography via UITheme constants
 *
 * Ownership: dialog is parented to the host Component; it self-deletes on
 * dismissal via Component::SafePointer.
 */
class Sp3ctraDialog final : public juce::Component
{
public:
    enum class Kind
    {
        Info,    ///< Single OK button, amber triangle (information).
        Warning, ///< Single OK button, red triangle (warning).
        Confirm, ///< Confirm + Cancel buttons, red triangle (destructive).
        Input    ///< Text-input field + OK / Cancel buttons.
    };

    /// Show an information dialog with a single OK button.
    static void showInfo(juce::Component* parent,
                         const char*      title,
                         const char*      message,
                         std::function<void()> onClose = {})
    {
        show(parent, Kind::Info, title, message,
             "OK", nullptr,
             [cb = std::move(onClose)](bool) { if (cb) cb(); });
    }

    /// Show a warning dialog with a single OK button.
    static void showWarning(juce::Component* parent,
                            const char*      title,
                            const char*      message,
                            std::function<void()> onClose = {})
    {
        show(parent, Kind::Warning, title, message,
             "OK", nullptr,
             [cb = std::move(onClose)](bool) { if (cb) cb(); });
    }

    /// Show a confirmation dialog with a confirm and a cancel button.
    /// @param onResult Called with true if the user confirmed, false otherwise.
    static void showConfirm(juce::Component*          parent,
                            const char*               title,
                            const char*               message,
                            const char*               confirmLabel,
                            const char*               cancelLabel,
                            std::function<void(bool)> onResult)
    {
        show(parent, Kind::Confirm, title, message,
             confirmLabel, cancelLabel, std::move(onResult));
    }

    /// Show a text-input dialog (OK / Cancel).
    /// @param onResult Called with the entered text when the user confirms;
    ///                 not called if the user cancels.
    static void showInput(juce::Component*                          parent,
                          const char*                               title,
                          const char*                               message,
                          const juce::String&                       defaultText,
                          const char*                               confirmLabel,
                          const char*                               cancelLabel,
                          std::function<void(const juce::String&)>  onResult)
    {
        if (parent == nullptr) return;

        constexpr int dw = 360;
        constexpr int dh = 170;

        auto* dlg = new Sp3ctraDialog(Kind::Input, title, message,
                                      confirmLabel, cancelLabel);
        dlg->input_->setText(defaultText, juce::dontSendNotification);
        dlg->input_->selectAll();

        dlg->confirmBtn_.onClick = [dlg, cb = onResult]()
        {
            const juce::String text = dlg->input_->getText();
            if (cb) cb(text);
            dlg->dismiss();
        };
        if (dlg->cancelBtn_ != nullptr)
        {
            dlg->cancelBtn_->onClick = [dlg]() { dlg->dismiss(); };
        }

        parent->addAndMakeVisible(dlg);
        dlg->setBounds((parent->getWidth()  - dw) / 2,
                       (parent->getHeight() - dh) / 2,
                       dw, dh);
        dlg->toFront(true);
        dlg->input_->grabKeyboardFocus();
    }

    void paint(juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();

        // ── Panel background + border ────────────────────────────────────────
        g.setColour(juce::Colour(0xf21e1e1e));
        g.fillRoundedRectangle(b, 5.0f);
        g.setColour(juce::Colour(0xff663322));
        g.drawRoundedRectangle(b.reduced(0.5f), 5.0f, 1.0f);

        // ── Warning / info triangle (hand-drawn, no OS icon) ─────────────────
        constexpr float tw = 18.0f, th = 16.0f;
        constexpr float tx = 14.0f, ty = 12.0f;
        juce::Path tri;
        tri.addTriangle(tx + tw * 0.5f, ty,
                        tx,             ty + th,
                        tx + tw,        ty + th);
        const juce::Colour triCol = (kind_ == Kind::Info)
                                        ? juce::Colour(0xffd9a441)
                                        : juce::Colour(0xffcc3311);
        g.setColour(triCol);
        g.fillPath(tri);
        g.setColour(juce::Colour(0xffffeeaa));
        g.setFont(juce::FontOptions(9.0f));
        g.drawText("!", (int) tx, (int) (ty + 3), (int) tw, (int) (th - 3),
                   juce::Justification::centred);

        // ── Title ────────────────────────────────────────────────────────────
        g.setColour(juce::Colour(0xffdde3e8));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        g.drawText(title_,
                   getLocalBounds().withY(10).withHeight(20),
                   juce::Justification::centredTop);

        // ── Message ──────────────────────────────────────────────────────────
        g.setColour(juce::Colour(0xff8a9aaa));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        const int msgHeight = (kind_ == Kind::Input) ? 18 : (getHeight() - 32 - 12 - Sp3ctraTheme::kControlH - 12);
        const auto msgArea =
            getLocalBounds().reduced(12, 0).withY(32).withHeight(msgHeight);
        const auto msgJust = (kind_ == Kind::Input)
                                ? juce::Justification::centredLeft
                                : juce::Justification::centred;
        g.drawFittedText(msg_, msgArea, msgJust, 4);
    }

    void resized() override
    {
        constexpr int pad  = 12;
        constexpr int gap  = 6;
        constexpr int btnH = Sp3ctraTheme::kControlH;
        const int by = getHeight() - pad - btnH;

        if (input_ != nullptr)
        {
            // Place the input field between the message and the buttons.
            const int inputY = 32 + 18 + 6;
            input_->setBounds(pad, inputY, getWidth() - 2 * pad, btnH);
        }

        if (cancelBtn_ != nullptr)
        {
            const int bw = (getWidth() - 2 * pad - gap) / 2;
            confirmBtn_.setBounds(pad,            by, bw, btnH);
            cancelBtn_->setBounds(pad + bw + gap, by, bw, btnH);
        }
        else
        {
            // Single OK button — centred, fixed width.
            constexpr int bw = 96;
            confirmBtn_.setBounds((getWidth() - bw) / 2, by, bw, btnH);
        }
    }

private:
    Sp3ctraDialog(Kind        kind,
                  const char* title,
                  const char* message,
                  const char* confirmLabel,
                  const char* cancelLabel)
        : kind_(kind),
          title_(title),
          msg_(message)
    {
        using B = juce::TextButton;

        confirmBtn_.setButtonText(confirmLabel);
        confirmBtn_.setColour(B::buttonColourId,  juce::Colour(0xff3a1a1a));
        confirmBtn_.setColour(B::textColourOffId, juce::Colour(0xffff6644));
        addAndMakeVisible(confirmBtn_);

        if (cancelLabel != nullptr)
        {
            cancelBtn_ = std::make_unique<juce::TextButton>();
            cancelBtn_->setButtonText(cancelLabel);
            cancelBtn_->setColour(B::buttonColourId,  juce::Colour(0xff242424));
            cancelBtn_->setColour(B::textColourOffId, juce::Colour(0xff999999));
            addAndMakeVisible(*cancelBtn_);
        }

        if (kind == Kind::Input)
        {
            input_ = std::make_unique<juce::TextEditor>();
            input_->setColour(juce::TextEditor::backgroundColourId,
                              juce::Colour(0xff141414));
            input_->setColour(juce::TextEditor::outlineColourId,
                              juce::Colour(0xff3a3a3a));
            input_->setColour(juce::TextEditor::focusedOutlineColourId,
                              juce::Colour(0xff663322));
            input_->setColour(juce::TextEditor::textColourId,
                              juce::Colour(0xffdde3e8));
            input_->setColour(juce::TextEditor::highlightColourId,
                              juce::Colour(0xff3a1a1a));
            input_->setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
            input_->setIndents(6, 4);
            input_->setMultiLine(false);
            input_->setReturnKeyStartsNewLine(false);
            input_->onReturnKey = [this]
            {
                if (confirmBtn_.onClick) confirmBtn_.onClick();
            };
            input_->onEscapeKey = [this]
            {
                if (cancelBtn_ != nullptr && cancelBtn_->onClick)
                    cancelBtn_->onClick();
            };
            addAndMakeVisible(*input_);
        }
    }

    static void show(juce::Component*          parent,
                     Kind                      kind,
                     const char*               title,
                     const char*               message,
                     const char*               confirmLabel,
                     const char*               cancelLabel,
                     std::function<void(bool)> onResult)
    {
        if (parent == nullptr) return;

        constexpr int dw = 360;
        constexpr int dh = 130;

        auto* dlg = new Sp3ctraDialog(kind, title, message,
                                      confirmLabel, cancelLabel);

        dlg->confirmBtn_.onClick = [dlg, cb = onResult]()
        {
            if (cb) cb(true);
            dlg->dismiss();
        };
        if (dlg->cancelBtn_ != nullptr)
        {
            dlg->cancelBtn_->onClick = [dlg, cb = onResult]()
            {
                if (cb) cb(false);
                dlg->dismiss();
            };
        }

        parent->addAndMakeVisible(dlg);
        dlg->setBounds((parent->getWidth()  - dw) / 2,
                       (parent->getHeight() - dh) / 2,
                       dw, dh);
        dlg->toFront(true);
    }

    void dismiss()
    {
        // Defer removal so the button's onClick finishes before we delete.
        juce::MessageManager::callAsync(
            [sp = juce::Component::SafePointer<Sp3ctraDialog>(this)]
            {
                if (sp != nullptr)
                {
                    if (auto* p = sp->getParentComponent())
                        p->removeChildComponent(sp.getComponent());
                    delete sp.getComponent();
                }
            });
    }

    Kind                              kind_;
    juce::String                      title_;
    juce::String                      msg_;
    juce::TextButton                  confirmBtn_;
    std::unique_ptr<juce::TextButton> cancelBtn_;
    std::unique_ptr<juce::TextEditor> input_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Sp3ctraDialog)
};
