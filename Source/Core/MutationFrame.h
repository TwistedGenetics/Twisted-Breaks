#pragma once

#include <array>
#include <cstdint>
#include "Core/EngineBridge.h"
#include "Core/SequencerData.h"
#include "Core/SliceSeqData.h"

namespace tb
{
    /*
        MutationFrame  (Phase 10)
        -------------
        A finished set of pattern changes, built on the message thread by the
        Mutator and handed to the audio thread, which writes it into the
        sequencer atomics at the exact boundary sample (the block is split
        there, like a snapshot switch). Plain numbers only: applying it is a
        few hundred atomic stores, no allocation.

            laneMask   main-sequencer lanes to replace (bit per pad)
            sliceMask  pads whose slice-sequencer slot `sliceSlot` to replace
            quant      params::SwitchAt: now / next beat / next bar
            bars       with quant = bar: every N bars from the song start
    */
    struct MutationFrame
    {
        int quant = 0;
        int bars  = 1;

        uint32_t laneMask = 0;
        std::array<std::array<uint64_t, SequencerData::kSteps>, kNumPads> seq {};

        uint32_t sliceMask = 0;
        std::array<int, kNumPads>      sliceSlot {};
        std::array<uint32_t, kNumPads> slotWord {};
        std::array<std::array<uint64_t, SliceSeqData::kSteps>, kNumPads> sliceSteps {};

        uint64_t retireEpoch = 0;

        /** Audio thread (or message thread with audio stopped). */
        void applyTo (SequencerData& main, SliceSeqData& slices) const noexcept
        {
            for (int p = 0; p < kNumPads; ++p)
            {
                if ((laneMask >> p) & 1u)
                    for (int k = 0; k < SequencerData::kSteps; ++k)
                        main.setPacked (p, k, seq[(size_t) p][(size_t) k]);
                if ((sliceMask >> p) & 1u)
                {
                    const int sl = sliceSlot[(size_t) p];
                    slices.setPackedSlot (p, sl, slotWord[(size_t) p]);
                    for (int k = 0; k < SliceSeqData::kSteps; ++k)
                        slices.setPackedStep (p, sl, k, sliceSteps[(size_t) p][(size_t) k]);
                }
            }
            if (laneMask != 0)  main.bumpVersion();
            if (sliceMask != 0) slices.bumpVersion();
        }
    };
}
