#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace tb::ui
{
    /*
        ReactorLookAndFeel
        ------------------
        Global look for stock JUCE widgets so anything added later inherits
        the skin automatically:
          * rotary sliders  -> black bakelite pointer knob with knurled skirt
          * linear sliders  -> slotted fader with a steel cap
          * tooltips        -> yellowed paper slip
    */
    class ReactorLookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        ReactorLookAndFeel();

        void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                               float startAngle, float endAngle, juce::Slider&) override;

        void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                               float minPos, float maxPos, juce::Slider::SliderStyle, juce::Slider&) override;

        void drawTooltip (juce::Graphics&, const juce::String& text, int w, int h) override;
        juce::Rectangle<int> getTooltipBounds (const juce::String& text, juce::Point<int> screenPos,
                                               juce::Rectangle<int> parentArea) override;

        juce::Label* createSliderTextBox (juce::Slider&) override;
    };
}
