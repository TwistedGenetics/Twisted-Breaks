#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <memory>
#include <vector>
#include <functional>
#include "Core/SnapshotFrame.h"

class TwistedBreaksProcessor;

namespace tb
{
    /*
        SnapshotManager  (message thread)
        ---------------
        8 snapshot slots. STORE captures every pad (sample, slices, all its
        settings), the group mixer, every effect (on / off + settings) and
        both sequencers; the master section and the bank stay live.

        RECALL (click, MIDI Program Change 1-8, or the automatable
        "Snapshot" parameter):
          1. the samples it needs are decoded in the background (shared with
             anything already loaded) - the slot shows LOADING
          2. a SnapshotFrame is built and armed - the slot shows WAITING
          3. the audio thread switches at the next boundary (NOW / NEXT BEAT /
             NEXT BAR), splitting the block so it lands on the exact sample
          4. back here: the parameters are written (so Live and the UI show
             the snapshot), the pads take over the buffers, the frame retires
    */
    class SnapshotManager final : private juce::Timer
    {
    public:
        static constexpr int kSlots = 8;
        enum class SlotState { empty, stored, loading, waiting, active };

        explicit SnapshotManager (TwistedBreaksProcessor&);
        ~SnapshotManager() override;

        void store (int slot);
        void clear (int slot);
        /** quant: params::SwitchAt, or -1 for the "Snapshot Switch" parameter's setting. */
        void recall (int slot, int quant = -1);

        /** A11 kit loads: switch to this snapshot-format tree (not stored in a slot)
            the same way, then call `landed` on the message thread. */
        void recallTree (const juce::ValueTree& snapshotTree, int quant, std::function<void()> landed);
        bool isRecallingTree() const noexcept { return externalPending; }

        /** Collect Samples: rewrite every stored sample path. */
        void rewritePaths (const std::function<juce::String (const juce::String&)>&);

        SlotState getState (int slot) const;
        int  getActive() const noexcept { return active; }
        /** A recall is loading or waiting for its boundary. */
        bool isSwitching() const noexcept { return loadingSlot >= 0 || waitingSlot >= 0; }
        int  framesInFlight() const noexcept { return (int) retiring.size(); }
        uint32_t getVersion() const noexcept { return version; }

        /** Shutdown: frames the processor still points at, deleted with the manager. */
        void adoptForDeletion (SnapshotFrame* f)
        {
            for (auto& r : retiring) if (r.get() == f) return;
            retiring.emplace_back (f);
        }

        /** Timer work, public so tests can drive it: write-back, retiring, MIDI / automation requests. */
        void poll();

        juce::ValueTree toValueTree() const;
        void fromValueTree (const juce::ValueTree&);
        static inline const juce::Identifier snapshotsId { "SNAPSHOTS" };

        /** Parameters a snapshot leaves alone (master, the snapshot / bank controls). */
        static bool isExcluded (const juce::String& paramId);

    private:
        void timerCallback() override { poll(); }
        std::unique_ptr<SnapshotFrame> buildFrame (int slot, int quant, const std::array<SampleBuffer::Ptr, kNumPads>& buffers);

        TwistedBreaksProcessor& processor;
        static constexpr int kExternal = kSlots;               // slots[kExternal]: a kit being loaded
        std::array<juce::ValueTree, kSlots + 1> slots;
        std::function<void()> externalLanded;
        bool externalPending = false;
        void startRecall (int slot, int quant);
        int active = -1, loadingSlot = -1, waitingSlot = -1;
        uint32_t requestGen = 0, version = 0;
        int lastParamValue = 0;

        std::vector<std::unique_ptr<SnapshotFrame>> retiring;

        JUCE_DECLARE_WEAK_REFERENCEABLE (SnapshotManager)
    };
}
