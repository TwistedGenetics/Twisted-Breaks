#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <array>
#include "Engine/SampleBuffer.h"
#include "Engine/SliceTable.h"

namespace tb
{
    class SampleManager;

    /*
        BreakBank  (message thread, spec addition A2)
        ---------
        8 slots, each a sliced break. Pads with FOLLOW BANK on play the
        ACTIVE slot's break (their own slice of it, or their slice sequence)
        instead of their own sample, so changing the active slot (the
        automatable "Break Bank" parameter, landing on the next beat / bar)
        swaps the break under every pad at once.

        Fill a slot by dropping a file on it (AUTO SLICE runs in the
        background) or copying the selected pad's sample + markers.
        Saved with the set; buffers are shared with the pads.
    */
    class BreakBank
    {
    public:
        static constexpr int kSlots = 8;
        enum class Status { empty, loading, ready, missing };

        struct Slot
        {
            Status status = Status::empty;
            juce::File file;
            juce::String name;
            SampleBuffer::Ptr buffer;
            SliceTable slices;
        };

        explicit BreakBank (SampleManager&);

        void loadFile (int slot, const juce::File&, const SliceTable* keepSlices = nullptr);
        void copyFromPad (int slot, int pad);
        void clear (int slot);
        /** Collect Samples: the slot now points at this copy of its file. */
        void repointFile (int slot, const juce::File& f) { if (juce::isPositiveAndBelow (slot, kSlots) && slots[(size_t) slot].file != juce::File()) { slots[(size_t) slot].file = f; ++version; } }

        Slot get (int slot) const;
        uint32_t getVersion() const noexcept { return version; }

        juce::ValueTree toValueTree() const;
        void fromValueTree (const juce::ValueTree&);
        static inline const juce::Identifier bankId { "BANK" };

    private:
        void install (int slot, const SampleBuffer::Ptr&, const SliceTable&);

        SampleManager& samples;
        std::array<Slot, kSlots> slots;
        std::array<uint32_t, kSlots> gen {};
        uint32_t version = 0;

        JUCE_DECLARE_WEAK_REFERENCEABLE (BreakBank)
    };
}
