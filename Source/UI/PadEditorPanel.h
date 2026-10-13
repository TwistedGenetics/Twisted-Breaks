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
        Controls for the SELECTED pad, under the pad matrix, on two pages:

            [PAD 07] [SOUND] [PLAY] [ sample readout ] [LOAD] [CLEAR]

            SOUND:  GAIN  PAN  PITCH  START  END  REVERSE
            PLAY:   MODE  SYNC  RATE  CHOKE  VEL  MUTE/SOLO  NOTE (+ LEARN)

        Every control is bound to that pad's automatable parameters
        (pad07_gain, pad07_mode ...), so the knobs, Ableton automation and
        the S / E markers on the screen always agree. Selecting another pad
        rebinds everything. The MIDI note is state, not a parameter.
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

        enum class Page { sound, play };
        void setPage (Page);
        Page getPage() const noexcept { return page; }

    private:
        void bindControls();
        void chooseFile();
        void layoutPages();
        juce::Rectangle<int> knobCell (int index) const;

        TwistedBreaksProcessor& processor;
        int  pad = 0;
        Page page = Page::sound;
        uint32_t shownVersion = 0xffffffff;

        // ---- SOUND page
        enum { kGain, kPan, kPitch, kStart, kEnd, kNumKnobs };
        std::array<juce::Slider, kNumKnobs> knobs;
        ToggleSwitch reverseSwitch { "REVERSE", "Play this pad's region backwards" };

        // ---- PLAY page
        RotarySelector modeSel  { { "ONE", "LOOP", "SLICE" } };
        ToggleSwitch   syncSwitch { "SYNC", "Loops follow Ableton's tempo and stay locked to the bar grid" };
        RotarySelector rateSel  { { "1/4", "1/2", "1x", "2x" } };
        RotarySelector chokeSel { { "OFF", "1", "2", "3", "4" } };
        juce::Slider   velKnob;
        PushKey        muteKey { "M", "Mute this pad", juce::Colour (0xffff3a1c) };
        PushKey        soloKey { "S", "Solo this pad (other pads go quiet)", juce::Colour (0xff7dff5a) };
        NoteSelector   noteSel;

        // ---- Header
        PushKey soundTab { "SOUND", "Level, pan, pitch, start / end, reverse" };
        PushKey playTab  { "PLAY",  "Mode, loop sync and rate, choke group, velocity, mute / solo, MIDI note" };
        PushKey loadKey  { "LOAD",  "Load a WAV / AIFF into the selected pad (or drag a file onto a pad)" };
        PushKey clearKey { "CLEAR", "Remove the sample from the selected pad" };

        using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
        using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
        std::array<std::unique_ptr<SliderAttachment>, kNumKnobs> knobAttachments;
        std::unique_ptr<SliderAttachment> velAttachment;
        std::unique_ptr<ButtonAttachment> reverseAttachment, syncAttachment, muteAttachment, soloAttachment;

        std::unique_ptr<juce::FileChooser> chooser;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadEditorPanel)
    };
}
