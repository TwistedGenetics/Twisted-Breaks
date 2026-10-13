#include "Engine/MasterSection.h"
#include "Core/Parameters.h"
#include <cmath>

namespace tb
{
    void MasterSection::prepare (double newSampleRate, int) noexcept
    {
        sampleRate  = newSampleRate;
        releaseCoef = (float) std::exp (-1.0 / (sampleRate * 0.080));   // 80 ms release
        gain.reset (sampleRate, 0.030);                                  // 30 ms level glide
        reset();
    }

    void MasterSection::reset() noexcept
    {
        envelope = 1.0f;
        gain.setCurrentAndTargetValue (gain.getTargetValue() > 0.0f ? gain.getTargetValue() : 1.0f);
    }

    void MasterSection::setGainDb (float db) noexcept
    {
        // Multiplicative smoothing can't reach 0, so -inf maps to a tiny value.
        const float lin = db <= params::masterGainMinDb + 0.01f ? 1.0e-5f
                                                                : juce::Decibels::decibelsToGain (db);
        gain.setTargetValue (lin);
    }

    MasterSection::Result MasterSection::process (juce::AudioBuffer<float>& buffer, int numSamples) noexcept
    {
        Result r;
        auto* left  = buffer.getWritePointer (0);
        auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

        float minEnv = 1.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float g = gain.getNextValue();
            float l = left[i] * g;
            float rr = right != nullptr ? right[i] * g : l;

            if (limiterOn)
            {
                const float level  = juce::jmax (std::abs (l), std::abs (rr));
                const float target = level > ceiling ? ceiling / level : 1.0f;

                // Instant attack, exponential release back toward unity.
                envelope = target < envelope ? target
                                             : target + (envelope - target) * releaseCoef;
                minEnv = juce::jmin (minEnv, envelope);

                l  = juce::jlimit (-ceiling, ceiling, l  * envelope);
                rr = juce::jlimit (-ceiling, ceiling, rr * envelope);
            }

            const float al = std::abs (l), ar = std::abs (rr);
            r.peakLeft  = juce::jmax (r.peakLeft,  al);
            r.peakRight = juce::jmax (r.peakRight, ar);
            if (al >= 1.0f || ar >= 1.0f) r.clipped = true;

            left[i] = l;
            if (right != nullptr) right[i] = rr;
        }

        r.maxReductionDb = minEnv < 1.0f ? -juce::Decibels::gainToDecibels (minEnv) : 0.0f;
        return r;
    }
}
