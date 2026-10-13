#include "Engine/PadVoice.h"
#include <cmath>

namespace tb
{
    namespace
    {
        constexpr int kMaxCrossfade = 256;   // source samples
    }

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

    double PadVoice::loopBeatsFor (double seconds) noexcept
    {
        if (seconds <= 0.0) return 4.0;
        double best = 4.0, bestDistance = 1.0e9;
        for (int k = -3; k <= 7; ++k)
        {
            const double beats = std::pow (2.0, (double) k);
            const double bpm   = beats * 60.0 / seconds;
            if (bpm >= 90.0 && bpm < 180.0) return beats;
            const double distance = std::abs (std::log2 (bpm / 127.0));
            if (distance < bestDistance) { bestDistance = distance; best = beats; }
        }
        return best;
    }

    void PadVoice::prepare (double rate) noexcept
    {
        hostRate        = rate > 0.0 ? rate : 44100.0;
        edgeFadeSamples = (float) juce::jmax (8.0, hostRate * 0.0013);
        increment.reset (hostRate, 0.02);
        gainL.reset (hostRate, 0.02);
        gainR.reset (hostRate, 0.02);
        kill();
    }

    int PadVoice::crossfadeLength() const noexcept
    {
        if (buffer == nullptr) return 0;
        const int len  = regionEnd - regionStart;
        const int room = reverse ? buffer->getNumSamples() - regionEnd   // audio after the region
                                 : regionStart;                         // audio before the region
        return juce::jmax (0, juce::jmin (kMaxCrossfade, room, len / 4));
    }

    double PadVoice::phaseLockedPosition (double ppq) const noexcept
    {
        const double len   = (double) (regionEnd - regionStart);
        double phase = ppq / syncPeriod;
        phase -= std::floor (phase);
        return reverse ? (double) regionEnd - 1.0 - phase * len
                       : (double) regionStart + phase * len;
    }

    void PadVoice::start (const SampleBuffer::Ptr& newBuffer, const Settings& s, float level,
                          double startPpq, bool hostPlaying) noexcept
    {
        if (newBuffer == nullptr || newBuffer->getNumSamples() <= 0) { kill(); return; }

        buffer       = newBuffer;
        active       = true;
        releasing    = false;
        reverse      = s.reverse;
        looping      = s.loop;
        syncPeriod   = s.loop ? s.syncPeriodBeats : 0.0;
        regionStart  = s.regionStart;
        regionEnd    = s.regionEnd;
        velocityGain = juce::jlimit (0.0f, 1.0f, level);
        releaseGain  = 1.0f;
        releaseStep  = 1.0f / juce::jmax (1.0f, (float) (hostRate * 0.005));

        position = reverse ? (double) (regionEnd - 1) : (double) regionStart;

        // A synced loop enters at the point the host's playhead says it should
        // be at, so it lands on the grid even when triggered mid-bar.
        const bool lockToHost = syncPeriod > 0.0 && hostPlaying;
        if (lockToHost) position = phaseLockedPosition (startPpq);

        // Starting exactly at the top of the file forwards keeps the attack
        // sample-exact; anywhere else gets a tiny fade so it can't click.
        const bool needsFade = reverse || regionStart > 0 || lockToHost;
        // Short (0.5 ms): slices start just before their attack, so the fade
        // is over before the hit and never softens it.
        fadeIn     = needsFade ? 0.0f : 1.0f;
        fadeInStep = 1.0f / juce::jmax (4.0f, (float) (hostRate * 0.0005));

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

        if (looping)
        {
            // Both edges are live for loops; keep the read head inside.
            syncPeriod  = s.syncPeriodBeats;
            regionStart = s.regionStart;
            regionEnd   = s.regionEnd;
            if (position < (double) regionStart || position >= (double) regionEnd)
                position = reverse ? (double) regionEnd - 1.0 : (double) regionStart;
        }
        else
        {
            // Only the far edge follows live edits for one-shots.
            if (reverse) regionStart = s.regionStart;
            else         regionEnd   = s.regionEnd;
        }
    }

    void PadVoice::syncTo (double ppq) noexcept
    {
        if (! active || ! looping || syncPeriod <= 0.0) return;

        const double len     = (double) (regionEnd - regionStart);
        const double desired = phaseLockedPosition (ppq);
        double err = desired - position;
        if (err >  len * 0.5) err -= len;
        if (err < -len * 0.5) err += len;

        // The increment is exact, so normally the error is a fraction of a
        // sample. Only a tempo change, a loop-brace wrap or a relocate moves
        // it by more — then snap back onto the grid.
        if (std::abs (err) > 2.0)
            position = desired;
    }

    void PadVoice::release (float fadeMs) noexcept
    {
        if (! active) return;
        releaseStep = 1.0f / juce::jmax (1.0f, (float) (hostRate * (double) fadeMs * 0.001));
        if (! releasing) releaseGain = 1.0f;
        releasing = true;
    }

    void PadVoice::kill() noexcept
    {
        active = releasing = looping = false;
        buffer = nullptr;          // safe on the audio thread: never the last reference
    }

    float PadVoice::renderAdding (juce::AudioBuffer<float>& out, int startSample, int numSamples) noexcept
    {
        if (! active) return 0.0f;

        const auto* buf    = buffer.get();
        const int   len    = buf->getNumSamples();
        const bool  stereo = buf->getNumChannels() > 1;
        const float* srcL  = buf->audio.getReadPointer (0);
        const float* srcR  = stereo ? buf->audio.getReadPointer (1) : srcL;

        float* outL = out.getWritePointer (0);
        float* outR = out.getNumChannels() > 1 ? out.getWritePointer (1) : nullptr;

        const int lo = juce::jlimit (0, len, regionStart);
        const int hi = juce::jlimit (lo, len, regionEnd);
        const double span = (double) (hi - lo);
        const int xf = looping ? crossfadeLength() : 0;

        float peak = 0.0f;

        for (int i = startSample; i < startSample + numSamples; ++i)
        {
            const double inc = increment.getNextValue();
            float env = fadeIn * velocityGain;
            if (fadeIn < 1.0f) fadeIn = juce::jmin (1.0f, fadeIn + fadeInStep);

            float l, r;

            if (looping && span >= 2.0)
            {
                // Wrap first (handles big increments and live region edits).
                while (position >= (double) hi) position -= span;
                while (position <  (double) lo) position += span;

                l = hermite (srcL, len, position);
                r = stereo ? hermite (srcR, len, position) : l;

                // Seam crossfade: blend toward the audio that would come next
                // after the wrap, so the jump back is inaudible.
                if (xf > 0)
                {
                    const double toEdge = reverse ? position - (double) lo : (double) hi - position;
                    if (toEdge < (double) xf)
                    {
                        const float  t   = 1.0f - (float) (toEdge / (double) xf);
                        const double alt = reverse ? (double) hi + (position - (double) lo)
                                                   : (double) lo - ((double) hi - position);
                        const float al = hermite (srcL, len, alt);
                        const float ar = stereo ? hermite (srcR, len, alt) : al;
                        l = l + (al - l) * t;
                        r = r + (ar - r) * t;
                    }
                }
            }
            else
            {
                // One-shot: distance to the far edge, in output samples.
                const double remaining = reverse ? (position - (double) lo + 1.0) / inc
                                                 : ((double) hi - position) / inc;
                if (remaining <= 0.0) { kill(); break; }
                if (remaining < (double) edgeFadeSamples) env *= (float) (remaining / (double) edgeFadeSamples);

                l = hermite (srcL, len, position);
                r = stereo ? hermite (srcR, len, position) : l;
            }

            if (releasing)
            {
                releaseGain -= releaseStep;
                if (releaseGain <= 0.0f) { kill(); break; }
                env *= releaseGain;
            }

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
