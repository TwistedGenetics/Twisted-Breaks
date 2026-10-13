#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <optional>

namespace tb
{
    /*
        TransportState
        --------------
        Immutable snapshot of the host's musical position at the FIRST sample
        of the current audio block, plus everything needed to locate any later
        sample in the block musically. Produced once per processBlock() by
        TransportSync and passed by const reference to every engine component
        that needs timing (sequencer, stutter, beat repeat, loop sync...).

        All positions are in PPQ (quarter notes since song start), which is
        what Ableton reports and what makes sample-accurate sync possible:
            ppqAt (sampleOffset) = ppqAtBlockStart + sampleOffset * ppqPerSample
    */
    struct TransportState
    {
        bool   hostProvidesInfo = false; // false in hosts with no playhead
        bool   isPlaying        = false;
        bool   justStarted      = false; // stopped -> playing on this block
        bool   justStopped      = false; // playing -> stopped on this block
        bool   jumped           = false; // locate / loop wrap while playing

        double bpm              = 120.0;
        double sampleRate       = 44100.0;
        double ppqAtBlockStart  = 0.0;
        double ppqPerSample     = 0.0;   // 0 when stopped
        int    numSamples       = 0;

        int    timeSigNumerator   = 4;
        int    timeSigDenominator = 4;
        double ppqOfBarStart      = 0.0;
        int    barIndex           = 0;   // 0-based (display adds 1)
        int    beatInBar          = 0;   // 0-based

        /** PPQ length of one bar for the current time signature. */
        double ppqPerBar() const noexcept
        {
            return (double) timeSigNumerator * 4.0 / (double) juce::jmax (1, timeSigDenominator);
        }

        /** Musical position of a given sample in this block. */
        double ppqAt (int sampleOffset) const noexcept
        {
            return ppqAtBlockStart + (double) sampleOffset * ppqPerSample;
        }

        /** Index of the 16th-note step (0..31) in a 32-step bar-aligned grid. */
        int stepAt (int sampleOffset, int stepsPerPattern = 32) const noexcept;

        /**
            Finds the first grid boundary of size `divisionPpq` (e.g. 0.25 for
            16ths, 4.0 for one 4/4 bar) that falls inside [fromSample, numSamples).
            Returns the sample offset, or std::nullopt if none falls in this
            block. This is the primitive the sequencer, stutter, beat repeat and
            quantised snapshot switching are all built on.
        */
        std::optional<int> nextBoundary (double divisionPpq, int fromSample = 0) const noexcept;
    };

    /*
        Values published from the audio thread for the UI to poll on its
        timer. Plain atomics: the UI only needs a consistent-enough view for
        display, and no locks ever touch the audio thread.
    */
    struct TransportUiState
    {
        std::atomic<bool>   hostProvidesInfo { false };
        std::atomic<bool>   isPlaying        { false };
        std::atomic<double> bpm              { 120.0 };
        std::atomic<double> ppq              { 0.0 };
        std::atomic<int>    bar              { 0 };
        std::atomic<int>    beat             { 0 };
        std::atomic<int>    step             { 0 };
        std::atomic<int>    timeSigNum       { 4 };
        std::atomic<int>    timeSigDen       { 4 };
    };

    /*
        TransportSync
        -------------
        Reads the host AudioPlayHead once per block and turns it into a
        TransportState. Also detects transport start/stop and jumps (Ableton
        loop-brace wraps, user relocating the playhead) by comparing where the
        host says we are with where we predicted we'd be.

        Runs ONLY on the audio thread. No allocation, no locks.
    */
    class TransportSync
    {
    public:
        void prepare (double sampleRate) noexcept;
        void reset() noexcept;

        /** Call at the very start of processBlock(). */
        const TransportState& update (juce::AudioPlayHead* playHead, int numSamples) noexcept;

        const TransportState& current() const noexcept { return state; }

        /** Copies the current state into the UI-visible atomics. */
        void publish (TransportUiState& ui) const noexcept;

    private:
        TransportState state;
        double sampleRate     = 44100.0;
        bool   wasPlaying     = false;
        double predictedPpq   = 0.0;    // where the next block should start
        bool   hasPrediction  = false;
    };
}
