#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <memory>
#include "UI/Controls.h"
#include "UI/SequencerPanel.h"      // DragReadout

class TwistedBreaksProcessor;

namespace tb::ui
{
    /*
        ResamplePanel  (RESAMPLE view, Phase 12)
        -------------
            [RESAMPLE]  SOURCE MASTER  LENGTH 2 BAR  START BAR  [AS LOOP] [AUTO SLICE]
            [ REC ]  [=========== RECORDING 2 BARS 43 % -> PAD 05 ======] [CANCEL]
            [CHARACTER]  PAD 05 - RESAMPLE ...
            [CLEAN] [SP-1200] [MPC60] [S950] [LO-FI]               [ALL PADS]
            26 kHz . 12 bit . no interpolation: gritty when pitched down

        REC records what you hear (or one group) onto the selected pad. The
        CHARACTER keys set the selected pad's vintage mode; ALL PADS copies
        it to every pad.
    */
    class ResamplePanel final : public juce::Component
    {
    public:
        explicit ResamplePanel (TwistedBreaksProcessor&);
        ~ResamplePanel() override;

        void setSelectedPad (int pad);
        void refresh();     // ~30 Hz

        void paint (juce::Graphics&) override;
        void resized() override;

        juce::Button& recButton()    { return recKey; }
        juce::Button& cancelButton() { return cancelKey; }
        juce::Button& allPadsButton() { return allKey; }
        juce::Button& characterButton (int c) { return *charKeys[(size_t) c]; }
        DragReadout& sourceReadout() { return sourceRead; }

    private:
        int characterOf (int pad) const;
        void setCharacter (int pad, int c);
        juce::String statusText() const;

        TwistedBreaksProcessor& processor;
        int selectedPad = 0;

        DragReadout sourceRead { "SOURCE", "What to record: the MASTER (what you hear) or one group" };
        DragReadout barsRead   { "LENGTH", "1, 2, 4 or 8 bars" };
        DragReadout startRead  { "START",  "Start on the next bar line, or straight away" };
        PushKey loopKey  { "AS LOOP",    "The new pad plays as a tempo-synced loop", juce::Colour (0xffffa928) };
        PushKey sliceKey { "AUTO SLICE", "Slice the new sample at its transients", juce::Colour (0xffffa928) };
        PushKey recKey    { "REC", "Record onto the selected pad (the Resample parameter, MIDI-mappable)", juce::Colour (0xffff3a1c) };
        PushKey cancelKey { "CANCEL", "Stop without keeping the take" };
        std::array<std::unique_ptr<PushKey>, 5> charKeys;
        PushKey allKey { "ALL PADS", "Give every pad this pad's CHARACTER" };
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> loopAtt, sliceAtt;

        juce::String shownStatus;
        int shownChar = -1;
        juce::Image background;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ResamplePanel)
    };
}
