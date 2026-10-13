#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace tb::ui
{
    /*
        FXGrid  —  FUTURE FEATURE (Phase 6), shown disconnected
        ------
        The 4 x 4 performance-FX key block. In Phase 1 the keys are drawn but
        deliberately dead: lamps dark, no mouse handling, and a "not
        connected" tag taped over it, so nothing on screen pretends to work.
        Phase 6 replaces the paint-only body with real LOCKED / MOMENTARY keys
        bound to per-group FX state.
    */
    class FXGrid final : public juce::Component,
                          public juce::SettableTooltipClient
    {
    public:
        FXGrid();

        void paint (juce::Graphics&) override;
        void resized() override;

        static const juce::StringArray& effectNames();

    private:
        juce::Image cache;
    };
}
