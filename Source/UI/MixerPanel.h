#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include "Core/EngineBridge.h"
#include "UI/Controls.h"

class TwistedBreaksProcessor;

namespace tb::ui
{
    /*
        MixerPanel  (MIXER view, Phase 5)
        ----------
        Four channel strips, one per group bus, in the space the CRT uses:

            [GROUP A]   header plate: click to send the SELECTED pad here;
             3 PADS     lamp lit when the selected pad is in this group
             ||  |  (PAN)
             ||  |  [ M ]      fader = grp_a_level, LED bar = post-fader peak
             ||  |  [ S ]
             -3.0   [BYPASS]   FX bypass (the FX chain arrives in Phase 6)

        Every control is bound to its automatable parameter.
    */
    class MixerPanel final : public juce::Component
    {
    public:
        explicit MixerPanel (TwistedBreaksProcessor&);
        ~MixerPanel() override;

        void setSelectedPad (int pad) { selectedPad = pad; repaint(); }
        void refresh();   // ~30 Hz from MainView

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseUp (const juce::MouseEvent&) override;

        /** Strip header plate in panel coordinates (exposed for tests). */
        juce::Rectangle<int> headerBounds (int group) const;

    private:
        juce::Rectangle<int> stripBounds (int group) const;
        juce::Rectangle<int> meterBounds (int group) const;
        int  padsInGroup (int group) const;
        int  groupOfPad (int pad) const;

        TwistedBreaksProcessor& processor;
        int selectedPad = 0;

        struct Strip
        {
            juce::Slider fader, pan;
            PushKey mute   { "M", "Mute this group", juce::Colour (0xffff3a1c) };
            PushKey solo   { "S", "Solo this group (other groups go quiet)", juce::Colour (0xff7dff5a) };
            PushKey bypass { "BYPASS", "Bypass this group's FX chain (the FX arrive in Phase 6)", juce::Colour (0xffff3a1c) };
            std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> faderAtt, panAtt;
            std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> muteAtt, soloAtt, bypassAtt;
            float meter = 0.0f;      // displayed level, 0..1 on the LED scale
            int   padCount = -1;
        };
        std::array<Strip, kNumGroups> strips;
        juce::Image background;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerPanel)
    };
}
