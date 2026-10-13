#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace tb
{
    /*
        MasterSection
        -------------
        Last stage before the host:  smoothed output level -> optional
        zero-latency peak limiter -> safety clip -> metering.

        The limiter has NO look-ahead on purpose: a live performance sampler
        must report zero latency so Ableton's pad response stays tight. It
        uses instant attack + smooth release, followed by a hard ceiling so
        nothing above -0.3 dBFS can ever leave the plugin when it is on.
    */
    class MasterSection
    {
    public:
        void prepare (double sampleRate, int maxBlockSize) noexcept;
        void reset() noexcept;

        void setGainDb (float db) noexcept;
        void setLimiterEnabled (bool shouldBeOn) noexcept { limiterOn = shouldBeOn; }

        struct Result
        {
            float peakLeft  = 0.0f;
            float peakRight = 0.0f;
            float maxReductionDb = 0.0f;
            bool  clipped   = false;   // a sample hit/exceeded 0 dBFS
        };

        Result process (juce::AudioBuffer<float>& buffer, int numSamples) noexcept;

    private:
        double sampleRate = 44100.0;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> gain;
        bool  limiterOn    = true;
        float envelope     = 1.0f;     // current limiter gain (linear)
        float releaseCoef  = 0.999f;

        static constexpr float ceiling = 0.966f;   // -0.3 dBFS
    };
}
