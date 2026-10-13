#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include "UI/Controls.h"
#include "Core/EngineBridge.h"

class TwistedBreaksProcessor;

namespace tb::ui
{
    /*
        PadEditorPanel
        --------------
        Controls for the SELECTED pad, under the pad matrix, on three pages:

            [PAD 07] [SOUND] [PLAY] [SLICE] [SWAP] [ readout ] [LOAD] [CLEAR]

            SOUND:  GAIN  PAN  PITCH  START  END  REVERSE  GROUP (A-D)
            PLAY:   MODE  SYNC  RATE  CHOKE  VEL  FIXED  MUTE/SOLO  NOTE (+ LEARN)
            SLICE:  AUTO / EQUAL  SENS  COUNT  SLICE  RESET / UNSLICE  SPREAD / FIT
            SWAP:   GROUP  AT  (+ how it works)

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

        enum class Page { sound, play, slice, swap };
        void setPage (Page);
        Page getPage() const noexcept { return page; }
        std::function<void (Page)> onPageChanged;
        std::function<void()>      onFitView;      // SLICE page FIT key

    private:
        void bindControls();
        void chooseFile();
        void layoutPages();
        std::vector<juce::Component*> sliceControls();
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
        PushKey        fixVelKey { "FIX", "FIXED VELOCITY: every hit plays at full level, however hard you play", juce::Colour (0xffffa928) };
        PushKey        muteKey { "M", "Mute this pad", juce::Colour (0xffff3a1c) };
        PushKey        soloKey { "S", "Solo this pad (other pads go quiet)", juce::Colour (0xff7dff5a) };
        NoteSelector   noteSel;

        // ---- SLICE page
        PushKey        autoKey   { "AUTO SLICE", "Find the hits in this break and put a slice marker on each (up to COUNT)" };
        PushKey        equalKey  { "EQUAL",      "Cut into COUNT equal slices" };
        juce::Slider   sensKnob;
        RotarySelector countSel  { { "4", "8", "16" } };
        juce::Slider   sliceKnob;
        PushKey        resetKey  { "RESET",   "Undo your marker edits: back to the last AUTO / EQUAL result" };
        PushKey        unsliceKey { "UNSLICE", "Remove all slice markers from this pad" };
        PushKey        spreadKey { "SPREAD",  "Put each slice on its own pad, starting here (pads in SLICE mode, ready to play from your controller)" };
        PushKey        fitKey    { "FIT",     "Zoom the screen out to the whole sample" };

        // ---- SOUND page: output group
        std::array<PushKey, kNumGroups> groupKeys { PushKey { "A", "Send this pad to group bus A" }, PushKey { "B", "Send this pad to group bus B" },
                                                    PushKey { "C", "Send this pad to group bus C" }, PushKey { "D", "Send this pad to group bus D" } };
        std::unique_ptr<juce::ParameterAttachment> groupAttachment;
        void showGroup (int g);

        // ---- SWAP page
        RotarySelector swapSel   { { "OFF", "1", "2", "3", "4" } };
        RotarySelector swapAtSel { { "SLICE", "1/16", "BEAT", "BAR" } };

        // ---- Header
        PushKey soundTab { "SOUND", "Level, pan, pitch, start / end, reverse" };
        PushKey playTab  { "PLAY",  "Mode, loop sync and rate, choke group, velocity, mute / solo, MIDI note" };
        PushKey sliceTab { "SLICE", "Find hits, edit slice markers, spread slices onto pads" };
        PushKey swapTab  { "SWAP",  "Swap between breaks at their slice points while they play" };
        PushKey loadKey  { "LOAD",  "Load a WAV / AIFF into the selected pad (or drag a file onto a pad)" };
        PushKey clearKey { "CLEAR", "Remove the sample from the selected pad" };

        using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
        using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
        std::array<std::unique_ptr<SliderAttachment>, kNumKnobs> knobAttachments;
        std::unique_ptr<SliderAttachment> velAttachment, sliceAttachment;
        std::unique_ptr<ButtonAttachment> reverseAttachment, syncAttachment, muteAttachment, soloAttachment, fixVelAttachment;

        std::unique_ptr<juce::FileChooser> chooser;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadEditorPanel)
    };
}
