#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include "UI/Controls.h"
#include "Core/EngineBridge.h"
#include "UI/SequencerPanel.h"

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
            SEQ:    SLICE SEQ on, SLOT, LEN, RATE, SWING (steps on the screen)

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

        enum class Page { sound, play, slice, swap, seq };
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
        uint32_t shownSliceVersion = 0xffffffff;

        // ---- SOUND page
        enum { kGain, kPan, kPitch, kStart, kEnd, kNumKnobs };
        std::array<juce::Slider, kNumKnobs> knobs;
        ToggleSwitch reverseSwitch { "REVERSE", "Play this pad's region backwards" };

        // ---- PLAY page
        RotarySelector modeSel  { { "ONE", "LOOP", "SLICE", "SEQ1", "SEQL", "CONT" } };   // A14: CONT = LOOP CONT
        // SEQ ONE / SEQ LOOP (A6): LAUNCH and LEN take SYNC / RATE's places.
        RotarySelector launchSel { { "NOW", "1/16", "BEAT", "BAR" } };
        DragReadout    seqLenRead { "LEN", "This pad's sequence length: GLOBAL (the sequencer's LEN) or 1-32 steps" };
        bool shownSeqMode = false;
        bool isSeqModeNow() const;
        // A13 (0.26): STRETCH in SYNC's old place (a LOOP pad's OFF / PITCH = the old SYNC off / on).
        RotarySelector stretchSel { { "OFF", "PITCH", "BEAT", "TEX", "AKAI", "AK.R" } };
        struct TempoChip final : public juce::Component, public juce::SettableTooltipClient
        {
            juce::String text { "--" };
            bool set = false;
            std::function<void()> onClick;
            void paint (juce::Graphics&) override;
            void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
        } tempoChip;
        juce::String shownTempoKey;
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
        PushKey        followBankKey { "FOLLOW BANK", "This pad plays the active BREAK BANK break (its slice of it) instead of its own sample", juce::Colour (0xff7dff5a) };
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> followBankAttachment;
        PushKey        swapFromKey { "FROM STEP 1", "A sequence taking over starts from its step 1 (off: it carries on from the same step)", juce::Colour (0xffffa928) };
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> swapFromAttachment;

        // ---- SEQ page (slice sequencer)
        PushKey     sseqKey  { "SLICE SEQ", "On: hitting this pad starts / stops its slice sequence (steps on the screen)", juce::Colour (0xff7dff5a) };
        DragReadout slotRead  { "SLOT",  "Pattern slot 1-8 (switches at the pattern start; automatable)" };
        DragReadout sLenRead  { "LEN",   "Pattern length in steps" };
        DragReadout sRateRead { "RATE",  "Step size: 1/8, 1/16, 1/32, 1/16T (click to change)" };
        DragReadout swingRead { "SWING", "Swing: delays every other step" };
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> sseqAttachment;
        std::vector<juce::Component*> seqControls();

        // ---- Header
        PushKey soundTab { "SOUND", "Level, pan, pitch, start / end, reverse" };
        PushKey playTab  { "PLAY",  "Mode, stretch, loop rate, choke group, velocity, mute / solo, MIDI note" };
        PushKey sliceTab { "SLICE", "Find hits, edit slice markers, spread slices onto pads" };
        PushKey swapTab  { "SWAP",  "Swap between breaks at their slice points while they play" };
        PushKey seqTab   { "SEQ",   "Slice sequencer: rearrange this pad's slices into a new pattern" };
        PushKey loadKey  { "LOAD",  "Load a WAV / AIFF into the selected pad (or drag a file onto a pad)" };
        PushKey clearKey { "CLEAR", "Remove the sample from the selected pad" };

        using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
        using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
        std::array<std::unique_ptr<SliderAttachment>, kNumKnobs> knobAttachments;
        std::unique_ptr<SliderAttachment> velAttachment, sliceAttachment;
        std::unique_ptr<ButtonAttachment> reverseAttachment, muteAttachment, soloAttachment, fixVelAttachment;

        std::unique_ptr<juce::FileChooser> chooser;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadEditorPanel)
    };
}
