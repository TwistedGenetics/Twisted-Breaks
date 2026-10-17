#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include "UI/Controls.h"
#include "Core/SequencerData.h"
#include <array>

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

        /** Or drive a plain value (state, not a parameter). */
        struct Model
        {
            std::function<int()> get;
            std::function<void (int)> set;
            int min = 0, max = 1, def = 0;
            std::function<juce::String (int)> text;
            bool cycleOnClick = false;
        };
        void bind (Model m) { attach (nullptr); model = std::move (m); repaint(); }
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
    private:
        void stepBy (int delta);
        juce::String caption;
        Model model;
        int modelStart = 0;
        juce::RangedAudioParameter* param = nullptr;
        std::unique_ptr<juce::ParameterAttachment> att;
        float startValue = 0.0f;
        bool dragged = false;
    };

    /*
        SequencerPanel  (SEQUENCER view, Phases 7 + 9)
        --------------
        The 32-step pattern, one lane per pad, over the transport + CRT area:

            [SEQUENCER] [RUN] LEN 16  STEP 1/16  REC Q 1/16  [ARM PAD 01] [CLEAR]
            01 |#..#|..#.|#..#|.... | ....
            02 |..#.|... ...                 amber = step on (fill = velocity)
            ...                              column glow = playhead
            PAD 01 STEP 05  VEL CHANCE RAT PITCH SLICE MICRO      <- the selected
            FX  COND  [REV] [HIT]  [FILL (HOLD)]   170.0  013.3.1    step (Phase 9)

          * click a cell: step on / off (and select it); drag up / down on a
            step: velocity; right-click or shift-click: select without toggling
          * click a lane number: select that pad (editor, ARM, CLEAR follow)
          * the STEP row edits the selected step: CHANCE, RATCHET, PITCH,
            SLICE, MICRO, FX (held for the step; HIT off = FX only), COND
            (1:2 .. 4:4, FILL, FIRST ...), REV. Editing an empty step turns it on.
          * FILL (HOLD) = the automatable "Sequencer Fill" parameter
          * cell marks: ticks on top = ratchet, dot top-right = chance,
            green corner = pitch / reverse / slice, red corner = FX,
            cyan underline = condition, outline only = FX only (no hit)
          * MUTATE page (Phase 10, the tab under STEP): AMOUNT, what may
            change (SLICE PITCH PATT RAT REV FX CHNC), SCOPE (selected pad /
            all), AUTO every 1-8 bars, AT (now / beat / bar), DRIFT, and
            MUTATE / UNDO / RESET. See Core/Mutator.
    */
    class SequencerPanel final : public juce::Component
    {
    public:
        explicit SequencerPanel (TwistedBreaksProcessor&);
        ~SequencerPanel() override;

        void setSelectedPad (int pad);
        std::function<void (int)> onSelectPad;     // a lane number / a cell was clicked

        void refresh();     // ~30 Hz
        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;

        /** Cell / lane-label rectangles (exposed for tests). */
        juce::Rectangle<float> cellBounds (int pad, int step) const;
        juce::Rectangle<float> laneLabelBounds (int pad) const;

        /** The step being edited by the STEP row. */
        void selectStep (int pad, int step);
        int  getSelectedStep() const noexcept { return selectedStep; }
        int  getSelectedPad() const noexcept  { return selectedPad; }

        /** STEP-row controls (tests drive them like a user would). */
        DragReadout& readout (const juce::String& caption);
        PushKey& revButton()  { return revKey; }
        PushKey& hitButton()  { return hitKey; }
        PushKey& fillButton() { return fillKey; }

        enum class Page { step, mutate };
        void showPage (Page);
        Page getPage() const noexcept { return page; }
        PushKey& mutateButton() { return goKey; }
        PushKey& undoButton()   { return undoKey; }
        PushKey& resetButton()  { return resetKey; }
        PushKey& targetButton (int t) { return *targetKeys[(size_t) t]; }

    private:
        juce::Rectangle<float> gridArea() const;
        juce::Rectangle<float> inspectorArea() const;
        bool cellAt (juce::Point<float>, int& pad, int& step) const;
        int  lengthNow() const;
        SeqStep currentStep() const;
        void writeStep (SeqStep);          // turns the step on
        void bindStepReadouts();
        void bindMutateControls();

        TwistedBreaksProcessor& processor;
        int selectedPad = 0, selectedStep = 0;

        DragReadout lenRead  { "LEN",   "Pattern length in steps (1-32): drag, scroll or double-click for 16" };
        DragReadout stepRead { "STEP",  "Step size: 1/8, 1/16 or 1/32 (click to change)" };
        DragReadout quantRead { "REC Q", "Live-record quantise: 1/4, 1/8, 1/16, 1/32 (click to change)" };
        PushKey     armKey   { "ARM", "Record the selected pad's hits (mouse or MIDI) into its lane while Live plays", juce::Colour (0xffff3a1c) };
        PushKey     clearKey { "CLEAR", "Clear the selected pad's lane" };

        // STEP row (Phase 9)
        DragReadout velRead    { "VEL",    "Velocity of the selected step" };
        DragReadout chanceRead { "CHANCE", "Probability the step plays (rolled each time round)" };
        DragReadout ratRead    { "RAT",    "Ratchet: 1-4 hits spread across the step (click to cycle)" };
        DragReadout pitchRead  { "PITCH",  "Pitch of this hit in semitones, on top of the pad's" };
        DragReadout sliceRead  { "SLICE",  "PAD = the pad's own sound, 1-16 = play that slice of its break" };
        DragReadout microRead  { "MICRO",  "Nudge the step early / late (1/128 step)" };
        DragReadout fxRead     { "FX",     "Effect held on the pad's group for the length of the step" };
        DragReadout condRead   { "COND",   "Condition: ALWAYS, A:B (pass A of every B), FILL, !FILL, FIRST, !FIRST" };
        PushKey revKey  { "REV",  "Play this hit backwards", juce::Colour (0xffffa928) };
        PushKey hitKey  { "HIT",  "On: the step plays the pad. Off: it only throws its FX", juce::Colour (0xffffa928) };
        PushKey fillKey { "FILL (HOLD)", "Hold: FILL steps play, !FILL steps rest (the automatable Sequencer Fill parameter)", juce::Colour (0xffff3a1c) };

        // Page tabs + MUTATE page (Phase 10)
        Page page = Page::step;
        PushKey stepTab   { "STEP",   "Edit the selected step" };
        PushKey mutateTab { "MUTATE", "Controlled randomisation of the patterns" };
        DragReadout amountRead { "AMOUNT", "How much MUTATE changes (0 % = the original)" };
        DragReadout scopeRead  { "SCOPE",  "PAD = the selected pad's lane / slice sequence, ALL = every pad" };
        DragReadout autoRead   { "AUTO",   "Mutate by itself every 1, 2, 4 or 8 bars, on the bar line (Mutate Auto)" };
        DragReadout atRead     { "AT",     "When the MUTATE key's change lands: now, next beat, next bar" };
        std::array<std::unique_ptr<PushKey>, 7> targetKeys;
        std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>, 7> targetAtts;
        PushKey driftKey { "DRIFT", "Off: every MUTATE is a fresh variation of your pattern. On: it builds on the last one", juce::Colour (0xffffa928) };
        PushKey goKey    { "GO", "MUTATE now / on the next beat / bar (the Mutate parameter, MIDI-mappable)", juce::Colour (0xffff3a1c) };
        PushKey undoKey  { "UNDO",  "Back to before the last change (again: redo)" };
        PushKey resetKey { "RESET", "Back to the original pattern" };
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> driftAtt;

        // Cell editing
        int  editPad = -1, editStep = -1;
        bool editWasOn = false, editDragged = false, editSelectOnly = false;
        int  editStartVel = 100;

        uint32_t shownVersion = 0xffffffff;
        int shownStep = -2, shownLen = -1, shownBar = -1, shownBeat = -1;
        uint32_t shownLaneHash = 0;
        bool shownFill = false;
        juce::Image background;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SequencerPanel)
    };
}
