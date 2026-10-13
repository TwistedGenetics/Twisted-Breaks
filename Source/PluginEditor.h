#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "UI/ReactorLookAndFeel.h"
#include "UI/MainView.h"

/*
    TwistedBreaksEditor
    -------------------
    Hosts the MainView console and scales it uniformly. The window can be
    resized from 75% to 150% with a locked aspect ratio; MainView always lays
    out at its fixed logical size, so no component ever has to care.
*/
class TwistedBreaksEditor final : public juce::AudioProcessorEditor
{
public:
    explicit TwistedBreaksEditor (TwistedBreaksProcessor&);
    ~TwistedBreaksEditor() override;

    void paint (juce::Graphics&) override {}
    void resized() override;

private:
    tb::ui::ReactorLookAndFeel lookAndFeel;   // must outlive the view
    tb::ui::MainView view;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TwistedBreaksEditor)
};
