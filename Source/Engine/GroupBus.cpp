#include "Engine/GroupBus.h"
#include "Core/Parameters.h"

namespace tb
{
    void GroupBus::prepare (double sampleRate, int maxSpan)
    {
        capacity = juce::jmax (64, maxSpan);
        input.setSize (2, capacity, false, true, false);
        gainL.reset (sampleRate, 0.02);
        gainR.reset (sampleRate, 0.02);
        reset();
    }

    void GroupBus::reset() noexcept
    {
        input.clear();
        used = false;
        spanLength = 0;
        jumpToParams = true;
    }

    void GroupBus::setParams (float levelDb, float pan, bool audible, bool bypass) noexcept
    {
        fxBypass = bypass;
        const float g = (! audible || levelDb <= params::grpLevelMinDb + 0.01f)
                          ? 0.0f : juce::Decibels::decibelsToGain (levelDb);
        float l, r;
        panGains (g, pan, l, r);
        if (jumpToParams)
        {
            gainL.setCurrentAndTargetValue (l);
            gainR.setCurrentAndTargetValue (r);
            jumpToParams = false;
        }
        else
        {
            gainL.setTargetValue (l);
            gainR.setTargetValue (r);
        }
    }

    juce::AudioBuffer<float>& GroupBus::beginSpan (int numSamples) noexcept
    {
        jassert (numSamples <= capacity);
        spanLength = juce::jmin (numSamples, capacity);
        if (used) input.clear (0, spanLength);   // only dirty if something played last span
        used = false;
        return input;
    }

    float GroupBus::mixInto (juce::AudioBuffer<float>& dest, int destStart, int numSamples) noexcept
    {
        numSamples = juce::jmin (numSamples, spanLength);

        // Nothing routed here this span (and no FX tails yet): just keep the
        // level glides moving so they stay in step with the clock.
        if (! used)
        {
            gainL.skip (numSamples);
            gainR.skip (numSamples);
            return 0.0f;
        }

        // ---- FX chain (Phase 6) goes here, skipped when fxBypass is set. ----

        const float* inL = input.getReadPointer (0);
        const float* inR = input.getReadPointer (1);
        float* outL = dest.getWritePointer (0, destStart);
        float* outR = dest.getNumChannels() > 1 ? dest.getWritePointer (1, destStart) : nullptr;

        float peak = 0.0f;
        if (! gainL.isSmoothing() && ! gainR.isSmoothing())
        {
            const float l = gainL.getCurrentValue(), r = gainR.getCurrentValue();
            for (int i = 0; i < numSamples; ++i)
            {
                const float vl = inL[i] * l, vr = inR[i] * r;
                outL[i] += vl;
                if (outR != nullptr) outR[i] += vr;
                peak = juce::jmax (peak, std::abs (vl), std::abs (vr));
            }
        }
        else
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float vl = inL[i] * gainL.getNextValue(), vr = inR[i] * gainR.getNextValue();
                outL[i] += vl;
                if (outR != nullptr) outR[i] += vr;
                peak = juce::jmax (peak, std::abs (vl), std::abs (vr));
            }
        }
        return peak;
    }
}
