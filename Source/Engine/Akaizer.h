#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>

namespace tb
{
    /*
        AkaiShifter (A12 part 5: AKAIZER)
        ------------
        Real-time cyclic pitch shift after the Akai S950 / S1000 / S3000
        "cyclic" time-stretch: the input is cut into cycles of CYCLE length,
        each cycle is played at the pitch ratio, and a new cycle starts from
        (a fixed delay behind) the live input every CYCLE.

          CLASSIC  splices the cycles almost hard (~0.5 ms): the cycle-rate
                   buzz and metallic phasing are the 90s sound itself.
          REVISED  crossfades 35 % of each cycle and starts each new cycle
                   where its waveform best matches the one fading out, so the
                   joins fall in phase: smoother, less buzz.

        The input is written into a ring all the time (also while the effect
        is off), so it engages with audio already in hand. Latency is about one
        cycle (x the pitch ratio when shifting up). No allocation after prepare().
    */
    class AkaiShifter
    {
    public:
        void prepare (double sampleRate)
        {
            sr = sampleRate > 0.0 ? sampleRate : 48000.0;
            int size = 1;
            while (size < (int) (sr * 1.5)) size <<= 1;              // 1.5 s: 150 ms cycles x 4 (+ search) fit
            ring.setSize (2, size, false, true, false);
            mask = size - 1;
            reset();
        }

        void reset() noexcept
        {
            ring.clear();
            written = 0;
            for (auto& h : heads) h = {};
            clock = 1 << 30;
        }

        /** pitch in semitones, cycle in ms. */
        void setParams (float pitchSemis, float cycleMs, bool revisedMode) noexcept
        {
            ratio   = std::pow (2.0, juce::jlimit (-36.0f, 36.0f, pitchSemis) / 12.0);
            cycle   = juce::jmax (32, (int) (juce::jlimit (2.0f, 400.0f, cycleMs) * 0.001 * sr));
            revised = revisedMode;
        }

        /** Every input sample goes in here (also while off). */
        void push (float l, float r) noexcept
        {
            const int i = (int) (written & mask);
            ring.setSample (0, i, l);
            ring.setSample (1, i, r);
            ++written;
        }

        /** The shifted sound for the sample just pushed. */
        void next (float& outL, float& outR) noexcept
        {
            const int x = revised ? juce::jmax (16, (int) (cycle * 0.35)) : juce::jmin (24, juce::jmax (4, cycle / 8));
            if (++clock >= cycle)
            {
                clock = 0;
                const double behind = cycle * juce::jmax (1.0, ratio) + x * ratio + (revised ? cycle * 0.5 : 0.0) + 16.0;
                double start = (double) written - behind;
                if (revised) start = bestStart (start, x);
                for (auto& h : heads) if (h.on) h.gstep = -juce::jmax (h.g, 1.0e-3f) / (float) x;
                spawn (start, x);
            }
            outL = outR = 0.0f;
            for (auto& h : heads)
            {
                if (! h.on) continue;
                const float g = h.g;
                h.g += h.gstep;
                if (h.g >= 1.0f) { h.g = 1.0f; h.gstep = 0.0f; }
                outL += read (0, h.pos) * g;
                outR += read (1, h.pos) * g;
                h.pos += ratio;
                if (h.gstep < 0.0f && h.g <= 0.0f) h.on = false;
            }
            if (std::abs (outL) < 1.0e-20f) outL = 0.0f;           // no denormals
            if (std::abs (outR) < 1.0e-20f) outR = 0.0f;
        }

        int latencySamples() const noexcept { return (int) (cycle * juce::jmax (1.0, ratio)); }

    private:
        struct Head { bool on = false; double pos = 0.0; float g = 0.0f, gstep = 0.0f; };

        float at (int ch, int64_t abs) const noexcept
        {
            if (abs < 0 || abs >= written || written - abs > (int64_t) mask) return 0.0f;
            return ring.getSample (ch, (int) (abs & mask));
        }
        float read (int ch, double pos) const noexcept
        {
            const auto i = (int64_t) std::floor (pos);
            const float f = (float) (pos - (double) i);
            return at (ch, i) + (at (ch, i + 1) - at (ch, i)) * f;
        }

        void spawn (double pos, int fadeIn) noexcept
        {
            int k = 0;
            for (int i = 0; i < (int) heads.size(); ++i) if (! heads[(size_t) i].on) { k = i; break; }
            if (heads[(size_t) k].on)
                for (int i = 1; i < (int) heads.size(); ++i) if (heads[(size_t) i].g < heads[(size_t) k].g) k = i;
            auto& h = heads[(size_t) k];
            h.on = true; h.pos = pos;
            h.g = 0.0f; h.gstep = 1.0f / (float) juce::jmax (1, fadeIn);
        }

        /** REVISED: the start (at or before `nominal`) whose next samples best match the fading cycle's. */
        double bestStart (double nominal, int x) const noexcept
        {
            int cur = -1;
            for (int i = 0; i < (int) heads.size(); ++i) if (heads[(size_t) i].on) cur = i;
            if (cur < 0) return nominal;
            const double from = heads[(size_t) cur].pos;
            const int W = juce::jlimit (16, 256, x);
            const int range = juce::jmax (8, cycle / 2);
            double best = nominal, bestScore = -1.0e30;
            for (int off = 0; off <= range; off += 3)
            {
                const double c = nominal - off;
                double score = 0.0;
                for (int k = 0; k < W; k += 4)
                    score += (double) read (0, from + k * ratio) * (double) read (0, c + k * ratio)
                           + (double) read (1, from + k * ratio) * (double) read (1, c + k * ratio);
                if (score > bestScore) { bestScore = score; best = c; }
            }
            return best;
        }

        double sr = 48000.0, ratio = 1.0;
        int cycle = 1200, clock = 1 << 30;
        bool revised = false;
        juce::AudioBuffer<float> ring;
        int mask = 0;
        int64_t written = 0;
        std::array<Head, 3> heads {};
    };
}
