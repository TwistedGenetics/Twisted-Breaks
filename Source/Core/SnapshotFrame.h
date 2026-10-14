#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <vector>
#include "Engine/AudioEngine.h"

namespace tb
{
    /*
        SnapshotFrame
        -------------
        Everything a snapshot changes, prepared on the message thread so the
        audio thread can switch to it in one go at the exact boundary sample:

          * params   - the engine's per-block view of every snapshot parameter
                       (pads, groups, FX, sequencer); used as an override
                       until the message thread has written the real
                       parameters, so the switch is sample-exact
          * raw / slices - every pad's own sample (already decoded) + markers
          * seq / slice  - both sequencers' data as packed numbers
          * values   - the parameters to write back (normalised)

        Lifetime: created by the SnapshotManager, handed over lock-free,
        applied by the audio thread, written back and retired by the
        manager; deleted only once the audio thread has moved on (epochs).
    */
    struct SnapshotFrame
    {
        int slot  = 0;
        int quant = 0;     // params::SwitchAt

        AudioEngine::BlockParams params;

        std::array<SampleBuffer::Ptr, kNumPads> buffers;       // references held here (message thread)
        std::array<SampleBuffer*, kNumPads>     raw {};        // the same, for the audio thread
        std::array<SliceTable, kNumPads>        slices {};

        std::array<std::array<uint32_t, SequencerData::kSteps>, kNumPads> seq {};
        std::array<std::array<std::array<uint64_t, SliceSeqData::kSteps>, SliceSeqData::kSlots>, kNumPads> sliceSteps {};
        std::array<std::array<uint32_t, SliceSeqData::kSlots>, kNumPads> sliceSlots {};

        std::vector<std::pair<juce::RangedAudioParameter*, float>> values;
        juce::ValueTree pads;

        uint64_t retireEpoch = 0;
    };
}
