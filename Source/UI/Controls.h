#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace tb::ui
{
    /** Bat-handle toggle switch on an ON / OFF plate, caption engraved below. */
    class ToggleSwitch final : public juce::Button
    {
    public:
        ToggleSwitch (const juce::String& caption, const juce::String& tooltip);
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    private:
        juce::String caption;
    };

    /** Momentary push key: black square key with a lamp that lights while held. */
    class PushKey final : public juce::Button
    {
    public:
        PushKey (const juce::String& label, const juce::String& tooltip);
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    };
}
