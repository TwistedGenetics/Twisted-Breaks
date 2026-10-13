#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include "UI/Controls.h"
#include "UI/SequencerPanel.h"
#include "Core/EngineBridge.h"

class TwistedBreaksProcessor;

namespace tb::ui
{
    /*
        SliceSeqPanel  (pad editor SEQ page -> screen area, spec addition A5)
        -------------
        The selected pad's slice sequence (its current SLOT):

            [SLICE SEQ]  PAD 01  SLOT 1  AMEN
            [ 1][ ~][ 3][ ~][ 5][ - ][ 2<][ ~]...   32 steps, 2 rows; - rest, ~ tie,
            [..]                                     < reversed, dots = ratchet
            SLICE PITCH VEL LEN RAT CHANCE MICRO [REV]   (the selected step)
            [FILL (hold)]                [SHUFFLE][RANDOM][REVERSE][HALF]
                                         [< ROT ][ ROT >][ RESET ][STEP REC]

          * click a step to select it; drag up / down on it to change its
            slice (down past 1: REST, then TIE); double-click: TIE
          * STEP REC: tap the pads of a SPREAD break (pads in SLICE mode on
            the same sample) to write their slices into the steps in turn
          * edits turn an untouched slot (the break as recorded) into steps
    */
    class SliceSeqPanel final : public juce::Component
    {
    public:
        explicit SliceSeqPanel (TwistedBreaksProcessor&);

        void setPad (int pad);
        void refresh();     // ~30 Hz

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        juce::Rectangle<float> cellBounds (int step) const;
        int getSelectedStep() const noexcept { return selected; }
        void selectStep (int k);

        /** Edit helpers (also used by tests). */
        SliceStep currentStep (int k) const;
        void writeStep (int k, const SliceStep&);

    private:
        int slot() const;
        int length() const;
        int sliceCount() const;
        void bindStepReadouts();
        static int encodeSlice (const SliceStep&);                  // -2 tie, -1 rest, 0..15 slice
        static void decodeSlice (SliceStep&, int v);

        TwistedBreaksProcessor& processor;
        int pad = 0, selected = 0;
        int dragStep = -1, dragStart = 0;
        bool dragged = false;

        DragReadout sliceRead { "SLICE", "Slice for this step (drag: 1..16, REST, TIE)" };
        DragReadout pitchRead { "PITCH", "Pitch of this step in semitones" };
        DragReadout velRead   { "VEL",   "Velocity of this step" };
        DragReadout lenRead   { "LEN",   "Gate: 1/4, 1/2, 3/4 of the step, or FULL (rings to the next hit)" };
        DragReadout ratRead   { "RAT",   "Ratchet: 1-4 hits inside the step (rolls)" };
        DragReadout chanceRead { "CHANCE", "Probability this step plays" };
        DragReadout microRead { "MICRO", "Nudge this step early / late (1/64 step)" };
        PushKey revKey     { "REV", "Play this step's slice backwards", juce::Colour (0xffffa928) };
        PushKey fillKey    { "FILL (HOLD)", "Hold for rolls: every hit ratchets x4, tied steps re-hit", juce::Colour (0xffff3a1c) };
        PushKey shuffleKey { "SHUFFLE", "Swap the hits around (the beats stay put)" };
        PushKey randomKey  { "RANDOM",  "Random slices on the off-beats" };
        PushKey reverseKey { "REVERSE", "Reverse the order of the steps" };
        PushKey halfKey    { "HALF",    "Repeat the first half" };
        PushKey rotLKey    { "< ROT",   "Rotate the pattern one step earlier" };
        PushKey rotRKey    { "ROT >",   "Rotate the pattern one step later" };
        PushKey resetKey   { "RESET",   "Back to the break as recorded" };
        PushKey stepRecKey { "STEP REC", "Tap SPREAD pads to write their slices into the steps in turn", juce::Colour (0xffff3a1c) };

        std::array<uint32_t, kNumPads> lastTriggers {};
        uint32_t shownVersion = 0xffffffff;
        int shownStep = -2;
        juce::Random rnd;
        juce::Image background;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SliceSeqPanel)
    };
}
