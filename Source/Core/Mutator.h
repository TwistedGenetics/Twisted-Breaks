#pragma once

#include <juce_events/juce_events.h>
#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>
#include "Core/MutationFrame.h"

class TwistedBreaksProcessor;

namespace tb
{
    /*
        Mutator  (Phase 10: MUTATE)
        -------
        Controlled randomisation of both sequencers, built so the groove
        survives:

          * hits ON the beat are anchors: never removed, moved, re-sliced,
            pitched or reversed (they may still get a ratchet, except the
            bar's downbeat)
          * new hits are only ghosts next to existing hits, at a fraction
            of the lane's velocity; nothing is ever moved off the grid
          * rolls (ratchets) and FX throws favour the last beat of the bar
          * slice-sequence MUTATE swaps slices between off-beat steps, so
            the break's own material is rearranged rather than replaced

        AMOUNT scales every chance; 0 % gives back the original exactly. By
        default each MUTATE is a fresh variation of the ORIGINAL (the pattern
        as you last wrote it: any edit, recording or snapshot recall becomes
        the new original); DRIFT builds on the last mutation instead.

        The result is a MutationFrame the audio thread swaps in at the exact
        boundary sample (MUTATE AT: now / next beat / next bar; AUTO: every
        1-8 bars, on the bar line). UNDO flips between the last two states;
        RESET goes back to the original. All message thread.
    */
    class Mutator final : private juce::Timer
    {
    public:
        explicit Mutator (TwistedBreaksProcessor&);
        ~Mutator() override;

        /** quant: params::SwitchAt, -1 = the "Mutate At" parameter. */
        void mutate (int quant = -1, int bars = 1);
        void undo();
        void reset();

        bool canUndo() const noexcept       { return undoFrame != nullptr; }
        bool hasOriginal() const noexcept   { return original != nullptr; }
        bool isWaiting() const noexcept     { return waiting; }
        int  getCount() const noexcept      { return count; }      // mutations applied (UI / tests)

        void setSelectedPad (int p) noexcept { selectedPad = juce::jlimit (0, kNumPads - 1, p); }
        void setSeed (juce::int64 s)         { rng.setSeed (s); }

        /** Timer work, public so tests can drive it. */
        void poll();
        void adoptForDeletion (MutationFrame* f) { if (f != nullptr) retiring.emplace_back (f); }

        // ---- The rules (pure, so they can be tested on their own) ---------------
        using Lane  = std::array<SeqStep, SequencerData::kSteps>;
        using Slots = std::array<SliceStep, SliceSeqData::kSteps>;
        static void mutateLane (Lane& out, const Lane& orig, int length, int stepsPerBeat, int sliceCount,
                                float amount, uint32_t targets, juce::Random&);
        static void mutateSlices (Slots& out, const Slots& orig, int length, int stepsPerBeat, int sliceCount,
                                  float amount, uint32_t targets, juce::Random&);

    private:
        void timerCallback() override { poll(); }
        uint32_t padsInScope() const;
        uint32_t sliceSeqPads() const;
        std::unique_ptr<MutationFrame> capture (uint32_t laneMask, uint32_t sliceMask) const;
        void refreshOriginal();
        void arm (std::unique_ptr<MutationFrame>);
        int  stepsPerBeatMain() const;

        TwistedBreaksProcessor& processor;
        std::unique_ptr<MutationFrame> original, undoFrame;
        std::vector<std::unique_ptr<MutationFrame>> retiring;
        uint32_t seenSeqVersion = 0, seenSliceVersion = 0;
        bool  waiting = false, lastGo = false;
        int   count = 0;
        int   lastAutoKey = -1;
        double lastAutoPpq = -1.0;
        int   selectedPad = 0;
        uint64_t lastEpoch = 0;
        int   stalledPolls = 0;
        juce::Random rng;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Mutator)
    };
}
