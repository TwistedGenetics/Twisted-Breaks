#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include "UI/Controls.h"

class TwistedBreaksProcessor;

namespace tb::ui
{
    /*
        PadEditorPanel
        --------------
        Controls for the SELECTED pad, under the pad matrix:

            [PAD 07] [ sample name / length / rate ] [LOAD] [CLEAR]
              GAIN   PAN   PITCH   START   END    REVERSE

        Every control is bound to that pad's automatable parameters
        (pad07_gain, ...), so moving a knob, drawing automation in Ableton or
        dragging the S / E markers on the CRT all move the same value.
        Selecting another pad rebinds the controls.
    */
    class PadEditorPanel final : public juce::Component
    {
    public:
        explicit PadEditorPanel (TwistedBreaksProcessor&);
        ~PadEditorPanel() override;

        void setPad (int pad);
        void refresh();     // ~30 Hz from MainView

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        void bindControls();
        void chooseFile();
        juce::Rectangle<int> knobCell (int index) const;

        TwistedBreaksProcessor& processor;
        int pad = 0;
        uint32_t shownVersion = 0xffffffff;

        enum { kGain, kPan, kPitch, kStart, kEnd, kNumKnobs };
        std::array<juce::Slider, kNumKnobs> knobs;
        ToggleSwitch reverseSwitch { "REVERSE", "Play this pad's region backwards" };
        PushKey loadKey  { "LOAD",  "Load a WAV / AIFF into the selected pad (or drag a file onto a pad)" };
        PushKey clearKey { "CLEAR", "Remove the sample from the selected pad" };

        using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
        using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
        std::array<std::unique_ptr<SliderAttachment>, kNumKnobs> knobAttachments;
        std::unique_ptr<ButtonAttachment> reverseAttachment;

        std::unique_ptr<juce::FileChooser> chooser;
        juce::Image background;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadEditorPanel)
    };
}
