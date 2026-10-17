#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <vector>
#include "Core/SampleManager.h"
#include "Core/SequencerData.h"
#include "Core/SliceSeqData.h"

class TwistedBreaksProcessor;

namespace tb
{
    /*
        PadOps (A11 part 2)
        -------------------
        Copy / paste / swap / clear between pads, and the undo history.

        A pad's content has three parts:
          * SAMPLE   — the audio (shared buffer, never re-decoded), markers,
                       detected markers, sensitivity, slice count
          * SETTINGS — every "padNN_*" parameter except mute and solo
          * STEPS    — its main sequencer lane and its slice sequencer (8 slots)
        The MIDI note is never copied.

        Undo: paste / swap / clear / preset loads are recorded straight away.
        Everything else that changes a pad's SAMPLE or STEPS (slice edits,
        step edits from the mouse, Mikro, Push or live recording, loading a
        file) is picked up by a watcher: once things have been quiet for a
        moment, the pads that differ from the last known state become one
        undo step. Parameter moves are left to the host's own undo.

        Message thread only.
    */
    class PadOps final : private juce::Timer
    {
    public:
        enum Part : uint32_t { sample = 1, settings = 2, steps = 4, sound = sample | settings, all = 7 };

        struct Content
        {
            uint32_t parts = 0;
            SampleManager::PadInfo info;                                       // sample
            std::vector<std::pair<juce::String, float>> params;               // settings: suffix -> 0..1
            std::array<uint64_t, SequencerData::kSteps> lane {};               // steps
            std::array<uint32_t, SliceSeqData::kSlots> slots {};
            std::array<std::array<uint64_t, SliceSeqData::kSteps>, SliceSeqData::kSlots> sliceSteps {};

            bool sameAs (const Content&, uint32_t parts) const;
        };

        explicit PadOps (TwistedBreaksProcessor&);
        ~PadOps() override;

        Content capture (int pad, uint32_t parts) const;
        void    apply (int pad, const Content&, uint32_t parts);

        // ---- Clipboard ---------------------------------------------------------------
        void copy (int pad);
        bool hasClip() const noexcept { return clip.parts != 0; }
        int  clipSource() const noexcept { return clipFrom; }
        /** Pastes the clipboard's `parts` onto `pad` (one undo step). */
        bool paste (int pad, uint32_t parts = all);
        /** Straight copy, no clipboard (Alt-drag, DUPLICATE). */
        void copyPad (int from, int to, uint32_t parts = all);
        void swap (int a, int b);
        void clear (int pad);

        // ---- Undo ----------------------------------------------------------------------
        bool canUndo() const noexcept { return index > 0; }
        bool canRedo() const noexcept { return index < (int) history.size(); }
        juce::String undoName() const { return canUndo() ? history[(size_t) index - 1].name : juce::String(); }
        juce::String redoName() const { return canRedo() ? history[(size_t) index].name : juce::String(); }
        bool undo();
        bool redo();
        int  historySize() const noexcept { return (int) history.size(); }
        int  position() const noexcept    { return index; }      // steps that can be undone
        static constexpr int kMaxHistory = 50;

        /** Records a change already made: `before` per pad, captured with `parts`. */
        struct PadChange { int pad; Content before, after; };
        void record (const juce::String& name, std::vector<PadChange> changes);

        /** Wraps a bigger edit (preset load …): captures every pad first, then call endEdit. */
        void beginEdit();
        void endEdit (const juce::String& name, uint32_t parts = all);

        /** Forget the history and take the current pads as the starting point (project load).
            Any thread (hops to the message thread). */
        void reset();

        /** The watcher (normally the timer; public for tests). */
        void poll (double nowMs);
        void stopWatchingForTesting() { stopTimer(); }
        static constexpr double kQuietMs = 600.0;

        /** Bumps on every history change (UI). */
        uint32_t getVersion() const noexcept { return version; }

    private:
        void timerCallback() override { poll (juce::Time::getMillisecondCounterHiRes()); }
        uint64_t watchStamp() const;
        void rebase (int pad);
        void rebaseAll();
        struct Transaction;
        void settle (const Transaction&);

        TwistedBreaksProcessor& processor;

        Content clip;
        int clipFrom = -1;

        struct Transaction { juce::String name; std::vector<PadChange> changes; };
        std::vector<Transaction> history;
        int index = 0;
        uint32_t version = 0;

        std::array<Content, SequencerData::kPads> baseline;    // sample + steps, last known
        uint64_t lastStamp = 0;
        double lastChangeMs = 0.0;
        bool dirty = false, needsRebase = true;

        std::vector<Content> editBefore;
        bool editing = false;

        static constexpr uint32_t kWatched = sample | steps;

        JUCE_DECLARE_WEAK_REFERENCEABLE (PadOps)
    };
}
