#include "NetworkSettingsTab.h"
#include "../Sp3ctraConstants.h"
#include "../UITheme.h"

//==============================================================================
NetworkSettingsTab::NetworkSettingsTab(Sp3ctraAudioProcessor& processor)
    : audioProcessor(processor),
      apvts(processor.getAPVTS())
{
    // UDP Port
    udpPortLabel.setText("UDP Port:", juce::dontSendNotification);
    udpPortLabel.setJustificationType(juce::Justification::centredRight);
    udpPortLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(udpPortLabel);

    udpPortEditor.setMultiLine(false);
    udpPortEditor.setReturnKeyStartsNewLine(false);
    udpPortEditor.setReadOnly(false);
    udpPortEditor.setScrollbarsShown(false);
    udpPortEditor.setCaretVisible(true);
    udpPortEditor.setPopupMenuEnabled(true);
    udpPortEditor.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    udpPortEditor.setJustification(juce::Justification::centred);
    udpPortEditor.setInputRestrictions(5, "0123456789");
    
    int currentPort = (int)apvts.getRawParameterValue("udpPort")->load();
    udpPortEditor.setText(juce::String(currentPort), false);
    
    // ✅ NO callbacks - changes only applied via "Apply Settings" button
    
    addAndMakeVisible(udpPortEditor);

    // UDP Address
    udpAddressLabel.setText("UDP Address:", juce::dontSendNotification);
    udpAddressLabel.setJustificationType(juce::Justification::centredRight);
    udpAddressLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(udpAddressLabel);

    // Helper lambda to configure IP byte editor
    auto setupByteEditor = [this](juce::TextEditor& editor, const char* paramName) {
        editor.setMultiLine(false);
        editor.setReturnKeyStartsNewLine(false);
        editor.setReadOnly(false);
        editor.setScrollbarsShown(false);
        editor.setCaretVisible(true);
        editor.setPopupMenuEnabled(true);
        editor.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        editor.setJustification(juce::Justification::centred);
        editor.setInputRestrictions(3, "0123456789");
        
        int currentValue = (int)apvts.getRawParameterValue(paramName)->load();
        editor.setText(juce::String(currentValue), false);
        
        // ✅ NO callbacks - changes only applied via "Apply Settings" button
        
        addAndMakeVisible(editor);
    };

    setupByteEditor(udpByte1Editor, "udpByte1");
    setupByteEditor(udpByte2Editor, "udpByte2");
    setupByteEditor(udpByte3Editor, "udpByte3");
    setupByteEditor(udpByte4Editor, "udpByte4");
    
    // Dot labels
    dot1Label.setText(".", juce::dontSendNotification);
    dot1Label.setJustificationType(juce::Justification::centred);
    dot1Label.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSettings)).boldened());
    addAndMakeVisible(dot1Label);

    dot2Label.setText(".", juce::dontSendNotification);
    dot2Label.setJustificationType(juce::Justification::centred);
    dot2Label.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSettings)).boldened());
    addAndMakeVisible(dot2Label);

    dot3Label.setText(".", juce::dontSendNotification);
    dot3Label.setJustificationType(juce::Justification::centred);
    dot3Label.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSettings)).boldened());
    addAndMakeVisible(dot3Label);

    // Sensor DPI
    sensorDpiLabel.setText("Sensor DPI:", juce::dontSendNotification);
    sensorDpiLabel.setJustificationType(juce::Justification::centredRight);
    sensorDpiLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(sensorDpiLabel);

    sensorDpiCombo.addItem("200 DPI (1728 pixels)", 1);
    sensorDpiCombo.addItem("400 DPI (3456 pixels)", 2);
    addAndMakeVisible(sensorDpiCombo);

    sensorDpiAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, "sensorDpi", sensorDpiCombo);

    // Apply Button
    applyButton.setButtonText("Apply Settings");
    applyButton.setEnabled(true);
    applyButton.onClick = [this]() { applyChanges(); };
    addAndMakeVisible(applyButton);

    // Status Label
    statusLabel.setText("Settings are saved automatically", 
                       juce::dontSendNotification);
    statusLabel.setJustificationType(juce::Justification::centred);
    statusLabel.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontTiny)).italicised());
    addAndMakeVisible(statusLabel);
}

NetworkSettingsTab::~NetworkSettingsTab()
{
}

void NetworkSettingsTab::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    // Section title
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSection)).boldened());
    g.drawText("Network Configuration", getLocalBounds().removeFromTop(30),
               juce::Justification::centred, true);
}

void NetworkSettingsTab::resized()
{
    auto bounds = getLocalBounds();
    bounds.removeFromTop(40);  // Skip title area
    bounds.reduce(20, 10);

    constexpr int labelWidth = Sp3ctraTheme::kLabelW;
    constexpr int rowHeight  = Sp3ctraTheme::kRowStep;
    constexpr int ctrlH      = Sp3ctraTheme::kControlH;
    constexpr int padding    = Sp3ctraTheme::kGap;

    // UDP Port
    {
        auto row = bounds.removeFromTop(rowHeight);
        udpPortLabel.setBounds(row.removeFromLeft(labelWidth).withSizeKeepingCentre(labelWidth, ctrlH));
        row.removeFromLeft(padding);
        udpPortEditor.setBounds(row.withSizeKeepingCentre(row.getWidth(), ctrlH));
    }
    bounds.removeFromTop(Sp3ctraTheme::kRowGap);

    // UDP Address
    {
        auto row = bounds.removeFromTop(rowHeight);
        udpAddressLabel.setBounds(row.removeFromLeft(labelWidth).withSizeKeepingCentre(labelWidth, ctrlH));
        row.removeFromLeft(padding);
        const int bw = (row.getWidth() - 24) / 4;
        udpByte1Editor.setBounds(row.removeFromLeft(bw).withSizeKeepingCentre(bw, ctrlH));
        dot1Label.setBounds(row.removeFromLeft(8));
        udpByte2Editor.setBounds(row.removeFromLeft(bw).withSizeKeepingCentre(bw, ctrlH));
        dot2Label.setBounds(row.removeFromLeft(8));
        udpByte3Editor.setBounds(row.removeFromLeft(bw).withSizeKeepingCentre(bw, ctrlH));
        dot3Label.setBounds(row.removeFromLeft(8));
        udpByte4Editor.setBounds(row.withSizeKeepingCentre(bw, ctrlH));
    }
    bounds.removeFromTop(Sp3ctraTheme::kRowGap);

    // Sensor DPI
    {
        auto row = bounds.removeFromTop(rowHeight);
        sensorDpiLabel.setBounds(row.removeFromLeft(labelWidth).withSizeKeepingCentre(labelWidth, ctrlH));
        row.removeFromLeft(padding);
        sensorDpiCombo.setBounds(row.withSizeKeepingCentre(row.getWidth(), ctrlH));
    }
    bounds.removeFromTop(Sp3ctraTheme::kSectionGap + Sp3ctraTheme::kRowGap);

    // Apply Button — standard control height, centred
    applyButton.setBounds(bounds.removeFromTop(Sp3ctraTheme::kRowStep)
                              .withSizeKeepingCentre(120, ctrlH));
    bounds.removeFromTop(Sp3ctraTheme::kRowGap);

    // Status label
    statusLabel.setBounds(bounds.removeFromTop(Sp3ctraTheme::kRowStep)
                              .withSizeKeepingCentre(bounds.getWidth(), ctrlH));
}

void NetworkSettingsTab::applyChanges()
{
    // ✅ FIX: Read values from TextEditor and apply ALL at once with batch update
    audioProcessor.beginUdpBatchUpdate();
    
    // Read and validate UDP Port
    int port = udpPortEditor.getText().getIntValue();
    if (port >= 1024 && port <= 65535) {
        apvts.getParameter("udpPort")->setValueNotifyingHost(
            apvts.getParameter("udpPort")->convertTo0to1(port));
    }

    int bytes[4] = {
        udpByte1Editor.getText().getIntValue(),
        udpByte2Editor.getText().getIntValue(),
        udpByte3Editor.getText().getIntValue(),
        udpByte4Editor.getText().getIntValue()
    };

    bool allZero = (bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0);
    if (allZero) {
        juce::String defaultAddress(Sp3ctraConstants::DEFAULT_UDP_ADDRESS);
        juce::StringArray parts;
        parts.addTokens(defaultAddress, ".", "");
        if (parts.size() == 4) {
            for (int i = 0; i < 4; ++i) {
                bytes[i] = parts[i].getIntValue();
            }

            udpByte1Editor.setText(juce::String(bytes[0]), false);
            udpByte2Editor.setText(juce::String(bytes[1]), false);
            udpByte3Editor.setText(juce::String(bytes[2]), false);
            udpByte4Editor.setText(juce::String(bytes[3]), false);
        }
    }

    // Read and validate UDP Address bytes
    auto applyByte = [this](int value, const char* paramName) {
        if (value >= 0 && value <= 255) {
            apvts.getParameter(paramName)->setValueNotifyingHost(
                apvts.getParameter(paramName)->convertTo0to1(value));
        }
    };
    
    applyByte(bytes[0], "udpByte1");
    applyByte(bytes[1], "udpByte2");
    applyByte(bytes[2], "udpByte3");
    applyByte(bytes[3], "udpByte4");
    
    // End batch - this will trigger a SINGLE UDP restart with all new parameters
    audioProcessor.endUdpBatchUpdate();
    
    updateStatusLabel();
    
    // Visual feedback
    applyButton.setButtonText(allZero ? "0.0.0.0 replaced" : "Settings Applied!");
    juce::Timer::callAfterDelay(1500, [this]() {
        applyButton.setButtonText("Apply Settings");
    });
}

void NetworkSettingsTab::updateStatusLabel()
{
    auto* core = audioProcessor.getSp3ctraCore();
    if (core && core->isInitialized()) {
        statusLabel.setText("Configuration active", juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, juce::Colours::green);
    } else {
        statusLabel.setText("Configuration error", juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, juce::Colours::red);
    }
}
