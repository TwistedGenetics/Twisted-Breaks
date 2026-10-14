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
        chain.prepare (sampleRate, capacity);
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

    float GroupBus::mixInto (juce::AudioBuffer<float>& dest, int destStart, int numSamples, const FxClock& clock,
                             juce::AudioBuffer<float>* tap) noexcept
    {
        numSamples = juce::jmin (numSamples, spanLength);

        // Nothing routed here this span and no effect on or ringing: just keep
        // the level glides and the FX clock / histories moving.
        if (! used && (fxBypass || chain.isIdle()))
        {
            gainL.skip (numSamples);
            gainR.skip (numSamples);
            chain.advanceSilent (numSamples, clock);
            return 0.0f;
        }

        // ---- FX chain, in place (FX BYPASS: audio skips it, histories still record).
        if (fxBypass) chain.recordOnly (input.getReadPointer (0), input.getReadPointer (1), numSamples, clock);
        else          chain.process (input.getWritePointer (0), input.getWritePointer (1), numSamples, clock);
        used = true;      // the input now holds effect output: clear it next span

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
        // RESAMPLE tap: the same output, added into the recorder's scratch.
        if (tap != nullptr && destStart + numSamples <= tap->getNumSamples())
        {
            const float l = gainL.getCurrentValue(), r = gainR.getCurrentValue();
            tap->addFrom (0, destStart, inL, numSamples, l);
            tap->addFrom (1, destStart, inR, numSamples, r);
        }
        return peak;
    }
}
