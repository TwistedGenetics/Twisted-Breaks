#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include "UI/Controls.h"
#include "UI/SequencerPanel.h"
#include "Core/MangleData.h"

class TwistedBreaksProcessor;

namespace tb::ui
{
    /*
        ManglePanel (A15: MANGLE)
        -----------
            MANGLE  [ON] [HOLD] [FILL NEXT]                    status   [FX]
            | RPT x2 | REV  | THRU | THRU | STOP ......... | SHUF 3 |   <- the bar
            [THRU][REPEAT][REVERSE][HALF][STOP][DOUBLE][SCRATCH][FREEZE][SHUFFLE]
            STEPS  TARGET  MIX  AMOUNT  FILL EVERY  FILL LEN
            [RANDOMIZE] [MUTATE] [<- REVERT]  STYLE           report

        Pick a move, then click or drag across steps to paint it (a drag joins
        the steps into one longer move; ALT-drag paints them one by one).
        Right-click a step: THRU / split / its REPEAT slices or SHUFFLE source.
        Mouse wheel on a step: REPEAT slices or SHUFFLE source.
    */
    class ManglePanel final : public juce::Component, private juce::Timer
    {
    public:
        explicit ManglePanel (TwistedBreaksProcessor&);
        ~ManglePanel() override;

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

        void refresh() { repaint(); }
        int  getBrush() const noexcept { return brush; }
        void setBrush (int m);

        std::function<void()> onBack;            // the FX key: back to the FX editor

        juce::Rectangle<float> stepRect (int step) const;      // one step cell (for tests)
        juce::Rectangle<float> runRect (int runStart) const;   // the whole run's cell
        int stepAt (juce::Point<float>) const;

        static juce::Colour moveColour (int move);

    private:
        void timerCallback() override;
        void setParam (const char* id, float value);
        void showStepMenu (int step);

        TwistedBreaksProcessor& processor;
        int brush = mangle::repeat;
        int dragFrom = -1, dragTo = -1;
        bool dragJoin = true;
        int shownStep = -2, shownSlot = -1;
        bool shownActive = false;
        uint32_t shownVersion = 0xffffffffu;
        int fillOffTicks = 0;

        PushKey onKey     { "ON", "MANGLE plays (with FILL EVERY off: always; on: only the fill bars)", juce::Colour (0xffff3a1c) };
        PushKey holdKey   { "HOLD", "Hold: mangles from the next step for as long as it's down (MIDI-map it to a pad)", juce::Colour (0xffff3a1c) };
        PushKey fillKey   { "FILL NEXT", "Mangles exactly one bar, from the next bar line", juce::Colour (0xffff3a1c) };
        PushKey backKey   { "FX", "Back to the FX editor" };
        PushKey randKey   { "RANDOMIZE", "Writes a whole new bar (listens to your last two bars first and skips any that click, vanish or run away)", juce::Colour (0xff7dff5a) };
        PushKey mutKey    { "MUTATE", "Changes a little of the bar", juce::Colour (0xff7dff5a) };
        PushKey revertKey { "< REVERT", "Back to the bar before the last RANDOMIZE / MUTATE (three deep)" };
        std::array<std::unique_ptr<PushKey>, mangle::kNumMoves> moveKeys;

        DragReadout stepsRead  { "STEPS",      "Steps a bar: 4, 8 or 16" };
        DragReadout targetRead { "TARGET",     "What MANGLE plays over: the master, or one group (after its FX)" };
        DragReadout mixRead    { "MIX",        "Dry / mangled" };
        DragReadout amountRead { "AMOUNT",     "Each step's chance to fire each bar (100 %: always)" };
        DragReadout everyRead  { "FILL EVERY", "Off: MANGLE plays every bar. 2-16: only the last bar of each phrase (turnarounds)" };
        DragReadout lenRead    { "FILL LEN",   "The fill is the whole last bar, or its second half" };
        DragReadout styleRead  { "STYLE",      "How much RANDOMIZE / MUTATE change: GENTLE, MEDIUM, WILD" };

        // 0.30: variations, TEXTURE, SPACE, presets
        std::array<std::unique_ptr<PushKey>, mangle::kSlots> slotKeys;
        PushKey chainKey   { "CHAIN", "Plays A, B, C ... up to the last filled variation, one bar each", juce::Colour (0xffd36bff) };
        PushKey texLock    { "TEXTURE", "Grains over the moves. Lit: locked (RANDOMIZE leaves TEXTURE as it is)" };
        PushKey spaceLock  { "SPACE", "Tempo-synced repeats of the mangled sound. Lit: locked (RANDOMIZE leaves SPACE as it is)" };
        PushKey presetKey  { "PRESETS", "Factory bars and phrases; save / load your own (.tbmangle)", juce::Colour (0xff7dff5a) };
        DragReadout texMode    { "MODE",    "TIGHT, SCATTER, CLOUD or REVERSE grains (OFF: none)" };
        DragReadout texAmount  { "AMOUNT",  "How much of the sound is grains" };
        DragReadout texSize    { "SIZE",    "Grain length" };
        DragReadout texDensity { "DENSITY", "Grains a second" };
        DragReadout texScatter { "SCATTER", "How far back, and how spread, the grains are" };
        DragReadout spMode     { "MODE",    "ECHO, PING-PONG or DIFFUSE repeats (OFF: none)" };
        DragReadout spTime     { "TIME",    "Repeat time, in time with Live" };
        DragReadout spFb       { "FEEDBACK", "How long the repeats go on (capped: it can't run away)" };
        DragReadout spAmount   { "AMOUNT",  "How loud the repeats are" };
        std::unique_ptr<juce::ButtonParameterAttachment> chainAtt, texLockAtt, spaceLockAtt;
        std::unique_ptr<juce::FileChooser> chooser;
        void showPresetMenu();
        void showSlotMenu (int slot);

        std::unique_ptr<juce::ButtonParameterAttachment> onAtt;
        juce::Image background;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ManglePanel)
    };
}
