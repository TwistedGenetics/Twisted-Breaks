#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <functional>

namespace tb::ui
{
    /*
        PadComponent
        ------------
        One of the 16 pads, styled as a backlit annunciator tile from a
        control-room alarm panel: engraved frosted glass in a screwed steel
        bezel that glows amber when the pad sounds.

        Interaction:
          * mouse down  -> trigger (velocity from click height: top = hard)
          * mouse up    -> release (used by gate/loop modes from Phase 2)
          * any click also selects the pad for the editor sections
    */
    class PadComponent final : public juce::Component,
                                 public juce::SettableTooltipClient
    {
    public:
        explicit PadComponent (int padIndex);

        // Callbacks wired by PadGrid
        std::function<void (int pad, float velocity)> onPress;
        std::function<void (int pad)>                 onRelease;
        std::function<void (int pad)>                 onSelect;

        void setActivity (float level);          // 0..1 from the audio engine
        void setSelected (bool shouldBeSelected);
        void setLabels (const juce::String& upper, const juce::String& name,
                        int midiNote, char group);

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;

    private:
        juce::Rectangle<float> glassArea() const;
        void rebuildOverlay();

        const int index;
        float activity  = 0.0f;
        bool  selected  = false;
        bool  pressed   = false;

        juce::String upperLabel, nameLabel;
        int  note  = 36;
        char group = 'A';

        juce::Image overlay;     // bezel, screws, grime, cracks — baked on resize

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadComponent)
    };
}
