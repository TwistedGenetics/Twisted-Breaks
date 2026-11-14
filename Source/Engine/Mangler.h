#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "Core/MangleData.h"
#include "Engine/FXChain.h"

namespace tb
{
    /*
        Mangler (A15: MANGLE)
        -------
        A step-sequenced buffer effect after quiver mangler. Everything that
        comes in is written into a ring (always, also while it's off), and
        each step of the bar plays it back one way:

          THRU     as it is                REPEAT   the run's first slice, cut 2 / 4 / 8
          REVERSE  the run before, back    HALF     half speed from the run start
          STOP     tape brake over the run DOUBLE   the run before, twice as fast (looped)
          SCRATCH  back and forth          FREEZE   holds the run's first moment
          SHUFFLE  another step of the last bar, in this step's place

        Zero latency: every move reads only audio already heard (a read that
        would be ahead of the input plays the input). Changes of move or run
        crossfade over 4 ms; loops crossfade into their own continuation, so
        nothing clicks.

        Gate (which steps play): ON, FILL EVERY n bars (the last bar or half
        bar of each phrase), HOLD (from the next step), FILL NEXT (one bar,
        from the next bar line). AMOUNT is each step's chance per bar.

        Allocates in prepare() only.
    */
    class Mangler
    {
    public:
        struct Gate
        {
            bool  on = false;
            bool  hold = false;
            int   fillEvery = 0;      // 0 off, else bars per phrase
            bool  fillHalf = false;   // FILL LENGTH: the last half bar (else the whole bar)
            int   fillNextCount = 0;  // bumps once per FILL NEXT press
            int   steps = 8;          // 4 / 8 / 16
            float amount = 1.0f;      // 0..1
            float mix = 1.0f;         // 0..1
        };

        void prepare (double sampleRate, int maxBlock);
        void reset() noexcept;

        /** In place. `clock` describes the first sample. */
        void process (float* left, float* right, int numSamples, const FxClock& clock,
                      const mangle::Pattern& pattern, const Gate& gate) noexcept;

        // ---- UI (relaxed reads) -------------------------------------------------------
        int  currentStep() const noexcept   { return uiStep.load (std::memory_order_relaxed); }
        bool currentActive() const noexcept { return uiActive.load (std::memory_order_relaxed); }

        /** Randomize's ears: asks the audio thread to copy the last `bars` bars it heard. */
        void requestCapture (int bars) noexcept { captureBars.store (bars); captureReq.fetch_add (1, std::memory_order_release); }
        bool captureReady (uint32_t& seen) const noexcept
        {
            const auto v = captureDone.load (std::memory_order_acquire);
            if (v == seen) return false;
            seen = v;
            return true;
        }
        uint32_t captureCount() const noexcept { return captureDone.load (std::memory_order_acquire); }
        /** Valid after captureReady(): the captured audio (starts on a bar line), its tempo and bar length. */
        const juce::AudioBuffer<float>& getCapture (int& length, double& bpm, double& samplesPerBar) const noexcept
        {
            length = capturedLength; bpm = capturedBpm; samplesPerBar = capturedBarSamples;
            return capture;
        }

        /** Offline: plays `in` (starting on a bar line) through the pattern with every step on,
            for randomize's checks and the tests. `out` gets the same length. */
        static void renderOffline (const mangle::Pattern&, int steps, const juce::AudioBuffer<float>& in, int length,
                                   double sampleRate, double bpm, double ppqPerBar, juce::AudioBuffer<float>& out);

    private:
        struct Map
        {
            int    move = mangle::thru;
            double start = 0.0;       // absolute sample index of the run start
            double len = 1.0;         // run length in samples
            double base = 0.0;        // SHUFFLE: absolute index of the source step's start
            int    slices = 2;        // REPEAT
            float  gain = 1.0f;       // trim for moves (not THRU)
        };

        float  readAt (int ch, double pos, int64_t now) const noexcept;
        void   value (const Map&, int64_t now, float liveL, float liveR, float& outL, float& outR) const noexcept;
        void   onStep (double ppq, double ppqPerSample, double ppqPerBar, int64_t now, int64_t bar, int step,
                       const mangle::Pattern&, const Gate&) noexcept;

        double sr = 48000.0;
        juce::AudioBuffer<float> ring;
        int64_t written = 0;
        int     ringSize = 1;

        Map cur, old;
        int xfPos = 0, xfLen = 192;

        int64_t lastBar = -1;
        int     lastStep = -1;
        bool    active = false;
        int64_t fillBar = -2;
        bool    fillPending = false;
        int     fillSeen = 0;
        uint32_t fireMask = 0xffffffffu;
        juce::Random rng { 0x7b3a };
        juce::SmoothedValue<float> mixSm;

        // capture
        juce::AudioBuffer<float> capture;
        std::atomic<uint32_t> captureReq { 0 }, captureDone { 0 };
        std::atomic<int> captureBars { 2 };
        uint32_t captureSeen = 0;
        int capturedLength = 0;
        double capturedBpm = 120.0, capturedBarSamples = 0.0;

        std::atomic<int>  uiStep { -1 };
        std::atomic<bool> uiActive { false };
    };
}
