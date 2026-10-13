#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <memory>
#include "UI/Controls.h"

class TwistedBreaksProcessor;

namespace tb::ui
{
    /*
        FxEditorPanel  (FX view, Phase 6)
        -------------
        Takes the CRT's place when the FX key in the top rail is pressed. Edits
        the effect picked on the FX grid, for the grid's group:

            FX EDITOR    GROUP A  ·  LPF - LOW PASS FILTER        [ON]
            (CUTOFF)  (RES)  (DRIVE)        [LOCKED] [MOMENTARY]  [ALL OFF]

        Knobs and switches are bound to that group's automatable parameters
        (grp_a_lpf_cutoff ...). LOCKED / MOMENTARY is saved with the set.
    */
    class FxEditorPanel final : public juce::Component
    {
    public:
        explicit FxEditorPanel (TwistedBreaksProcessor&);
        ~FxEditorPanel() override;

        void setTarget (int group, int fxId);
        void refresh();     // ~30 Hz

        void paint (juce::Graphics&) override;
        void resized() override;

        int getGroup() const noexcept { return group; }
        int getFx() const noexcept    { return fxId; }

    private:
        void rebind();
        juce::Rectangle<int> controlCell (int k) const;

        TwistedBreaksProcessor& processor;
        int group = 0, fxId = 1;

        std::array<juce::Slider, 3> knobs;
        std::array<std::unique_ptr<RotarySelector>, 3> selectors;
        std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>, 3> knobAtt;

        PushKey onKey        { "ON", "Switch this effect on / off for this group (automatable)" };
        PushKey lockedKey    { "LOCKED", "LOCKED: click (or a MIDI note) turns it on, again turns it off" };
        PushKey momentaryKey { "MOMENTARY", "MOMENTARY: on only while the key / MIDI note is held" };
        PushKey allOffKey    { "ALL OFF", "Switch every effect of this group off" };
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAtt;

        juce::Image background;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxEditorPanel)
    };
}
