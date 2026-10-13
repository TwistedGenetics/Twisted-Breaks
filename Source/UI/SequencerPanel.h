#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include "UI/Controls.h"

class TwistedBreaksProcessor;

namespace tb::ui
{
    /** Small VFD number you drag / scroll / click to change a parameter (int or choice). */
    class DragReadout final : public juce::Component,
                              public juce::SettableTooltipClient
    {
    public:
        DragReadout (const juce::String& caption, const juce::String& tooltip);
        void attach (juce::RangedAudioParameter*);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
    private:
        void stepBy (int delta);
        juce::String caption;
        juce::RangedAudioParameter* param = nullptr;
        std::unique_ptr<juce::ParameterAttachment> att;
        float startValue = 0.0f;
        bool dragged = false;
    };

    /*
        SequencerPanel  (SEQUENCER view, Phase 7)
        --------------
        The 32-step pattern, one lane per pad, in the CRT's place:

            [SEQUENCER] [RUN] LEN 16  STEP 1/16  REC Q 1/16  [ARM PAD 01] [CLEAR]
            01 |#..#|..#.|#..#|.... | ....
            02 |..#.|... ...                 amber = step on (fill = velocity)
            ...                              column glow = playhead

          * click a cell: step on / off; drag up / down on a step: velocity
          * click a lane number: select that pad (editor, ARM, CLEAR follow)
          * ARM: hits on the selected pad (mouse or MIDI) are recorded into
            its lane while Live plays, quantised to REC Q
          * LEN / STEP / REC Q / RUN are automatable parameters; the pattern
            itself is saved with the set
    */
    class SequencerPanel final : public juce::Component
    {
    public:
        explicit SequencerPanel (TwistedBreaksProcessor&);
        ~SequencerPanel() override;

        void setSelectedPad (int pad);
        std::function<void (int)> onSelectPad;     // a lane number was clicked

        void refresh();     // ~30 Hz
        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;

        /** Cell / lane-label rectangles (exposed for tests). */
        juce::Rectangle<float> cellBounds (int pad, int step) const;
        juce::Rectangle<float> laneLabelBounds (int pad) const;

    private:
        juce::Rectangle<float> gridArea() const;
        bool cellAt (juce::Point<float>, int& pad, int& step) const;
        int  lengthNow() const;

        TwistedBreaksProcessor& processor;
        int selectedPad = 0;

        PushKey     runKey   { "RUN", "Sequencer plays with Live's transport (automatable)", juce::Colour (0xff7dff5a) };
        DragReadout lenRead  { "LEN",   "Pattern length in steps (1-32): drag, scroll or double-click for 16" };
        DragReadout stepRead { "STEP",  "Step size: 1/8, 1/16 or 1/32 (click to change)" };
        DragReadout quantRead { "REC Q", "Live-record quantise: 1/4, 1/8, 1/16, 1/32 (click to change)" };
        PushKey     armKey   { "ARM", "Record the selected pad's hits (mouse or MIDI) into its lane while Live plays", juce::Colour (0xffff3a1c) };
        PushKey     clearKey { "CLEAR", "Clear the selected pad's lane" };
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> runAtt;

        // Cell editing
        int  editPad = -1, editStep = -1;
        bool editWasOn = false, editDragged = false;
        int  editStartVel = 100;

        uint32_t shownVersion = 0xffffffff;
        int shownStep = -2, shownLen = -1;
        juce::Image background;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SequencerPanel)
    };
}
