#include "Engine/PadVoice.h"
#include <cmath>

namespace tb
{
    float PadVoice::hermite (const float* d, int n, double pos) noexcept
    {
        const int   i = (int) std::floor (pos);
        const float f = (float) (pos - (double) i);

        auto at = [d, n] (int k) noexcept { return d[juce::jlimit (0, n - 1, k)]; };
        const float y0 = at (i - 1), y1 = at (i), y2 = at (i + 1), y3 = at (i + 2);

        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + y1;
    }

    void PadVoice::prepare (double rate) noexcept
    {
        hostRate        = rate > 0.0 ? rate : 44100.0;
        edgeFadeSamples = (float) juce::jmax (8.0, hostRate * 0.0013);
        releaseStep     = (float) (1.0 / juce::jmax (1.0, hostRate * 0.005));
        increment.reset (hostRate, 0.02);
        gainL.reset (hostRate, 0.02);
        gainR.reset (hostRate, 0.02);
        kill();
    }

    void PadVoice::start (const SampleBuffer::Ptr& newBuffer, const Settings& s, float velocity) noexcept
    {
        if (newBuffer == nullptr || newBuffer->getNumSamples() <= 0) { kill(); return; }

        buffer       = newBuffer;
        active       = true;
        releasing    = false;
        reverse      = s.reverse;
        regionStart  = s.regionStart;
        regionEnd    = s.regionEnd;
        velocityGain = juce::jlimit (0.0f, 1.0f, velocity);
        releaseGain  = 1.0f;

        position = reverse ? (double) (regionEnd - 1) : (double) regionStart;

        // Starting exactly at the top of the file forwards keeps the attack
        // sample-exact; anywhere else gets a tiny fade so it can't click.
        const bool needsFade = reverse || regionStart > 0;
        fadeIn     = needsFade ? 0.0f : 1.0f;
        fadeInStep = 1.0f / edgeFadeSamples;

        increment.setCurrentAndTargetValue (s.increment);
        gainL.setCurrentAndTargetValue (s.gainLeft);
        gainR.setCurrentAndTargetValue (s.gainRight);
    }

    void PadVoice::update (const Settings& s) noexcept
    {
        if (! active) return;
        increment.setTargetValue (s.increment);
        gainL.setTargetValue (s.gainLeft);
        gainR.setTargetValue (s.gainRight);

        // Only the far edge follows live edits; the near edge only matters at start.
        if (reverse) regionStart = s.regionStart;
        else         regionEnd   = s.regionEnd;
    }

    void PadVoice::release() noexcept
    {
        if (active) releasing = true;
    }

    void PadVoice::kill() noexcept
    {
        active = releasing = false;
        buffer = nullptr;          // safe on the audio thread: never the last reference
    }

    float PadVoice::renderAdding (juce::AudioBuffer<float>& out, int startSample, int numSamples) noexcept
    {
        if (! active) return 0.0f;

        const auto* buf   = buffer.get();
        const int   len   = buf->getNumSamples();
        const bool  stereo = buf->getNumChannels() > 1;
        const float* srcL = buf->audio.getReadPointer (0);
        const float* srcR = stereo ? buf->audio.getReadPointer (1) : srcL;

        float* outL = out.getWritePointer (0);
        float* outR = out.getNumChannels() > 1 ? out.getWritePointer (1) : nullptr;

        const int lo = juce::jlimit (0, len, regionStart);
        const int hi = juce::jlimit (lo, len, regionEnd);

        float peak = 0.0f;

        for (int i = startSample; i < startSample + numSamples; ++i)
        {
            const double inc = increment.getNextValue();

            // Distance to the far edge, in output samples.
            const double remaining = reverse ? (position - (double) lo + 1.0) / inc
                                             : ((double) hi - position) / inc;
            if (remaining <= 0.0) { kill(); break; }

            float env = fadeIn * velocityGain;
            if (fadeIn < 1.0f) fadeIn = juce::jmin (1.0f, fadeIn + fadeInStep);
            if (remaining < (double) edgeFadeSamples) env *= (float) (remaining / (double) edgeFadeSamples);

            if (releasing)
            {
                releaseGain -= releaseStep;
                if (releaseGain <= 0.0f) { kill(); break; }
                env *= releaseGain;
            }

            const float l = hermite (srcL, len, position);
            const float r = stereo ? hermite (srcR, len, position) : l;

            const float vl = l * env * gainL.getNextValue();
            const float vr = r * env * gainR.getNextValue();

            outL[i] += vl;
            if (outR != nullptr) outR[i] += vr;
            peak = juce::jmax (peak, std::abs (vl), std::abs (vr));

            position += reverse ? -inc : inc;
        }

        return peak;
    }
}
