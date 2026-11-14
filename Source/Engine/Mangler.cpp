#include "Engine/Mangler.h"

namespace tb
{
    namespace
    {
        constexpr double kRingSeconds = 12.0;    // 2 bars at 40 BPM
    }

    void Mangler::prepare (double sampleRate, int)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        ringSize = (int) std::ceil (sr * kRingSeconds);
        ring.setSize (2, ringSize, false, true, false);
        capture.setSize (2, ringSize, false, true, false);
        xfLen = juce::jmax (16, (int) (0.004 * sr));
        mixSm.reset (sr, 0.02);
        reset();
    }

    void Mangler::reset() noexcept
    {
        ring.clear();
        written = 0;
        cur = {}; old = {};
        xfPos = xfLen;
        lastBar = -1; lastStep = -1;
        active = false;
        fillBar = -2; fillPending = false;
        fireMask = 0xffffffffu;
        uiStep.store (-1); uiActive.store (false);
    }

    float Mangler::readAt (int ch, double pos, int64_t now) const noexcept
    {
        // Never ahead of the input: the newest sample is the live one.
        if (pos > (double) now) pos = (double) now;
        const auto i = (int64_t) std::floor (pos);
        const float f = (float) (pos - (double) i);
        auto at = [&] (int64_t k) -> float
        {
            if (k < 0 || k > now || now - k >= (int64_t) ringSize) return 0.0f;
            return ring.getSample (ch, (int) (k % ringSize));
        };
        const float a = at (i);
        return f > 0.0f ? a + (at (i + 1) - a) * f : a;
    }

    void Mangler::value (const Map& m, int64_t now, float liveL, float liveR, float& outL, float& outR) const noexcept
    {
        using namespace mangle;
        const double t = (double) now - m.start;          // samples into the run
        const double L = juce::jmax (8.0, m.len);
        double src = (double) now;
        float env = 1.0f;
        double blendSrc = -1.0;                            // loop crossfade into the continuation
        float blend = 0.0f;
        const double xf = juce::jmin (256.0, L * 0.125);

        auto loopFrom = [&] (double origin, double loopLen, double pos, bool wrapped)
        {
            // pos in [0, loopLen): after each wrap the loop's own continuation fades out over xf samples.
            src = origin + pos;
            const double x = juce::jmin (xf, loopLen * 0.25);
            if (wrapped && pos < x && x > 1.0)
            {
                blendSrc = origin + loopLen + pos;
                blend = 1.0f - (float) (pos / x);
            }
        };

        switch (m.move)
        {
            case thru: outL = liveL; outR = liveR; return;

            case repeat:
            {
                const double sl = L / (double) juce::jmax (1, m.slices);
                const double pos = std::fmod (juce::jmax (0.0, t), sl);
                src = m.start + pos;
                // A short gate on each repeat: crisp, click-free stutters.
                const double a = juce::jmin (48.0, sl * 0.1), r = juce::jmin (160.0, sl * 0.2);
                if (t >= sl && pos < a) env = (float) (pos / a);
                if (pos > sl - r)       env = juce::jmin (env, (float) ((sl - pos) / r));
                break;
            }
            case reverse:  src = m.start - 1.0 - t; break;
            case half:     src = m.start + t * 0.5; break;
            case stop:
            {
                const double u = juce::jlimit (0.0, L, t);
                src = m.start + u - u * u / (2.0 * L);
                const double tail = L * 0.12;
                if (t > L - tail) env = (float) juce::jmax (0.0, (L - t) / tail);
                break;
            }
            case dbl:      loopFrom (m.start - L, L, std::fmod (juce::jmax (0.0, 2.0 * t), L), 2.0 * t >= L); break;
            case scratch:
            {
                const double D = juce::jmin (L, sr * 0.35);
                const double u = t / L;
                src = m.start - D + D * 0.5 * (1.0 - std::cos (juce::MathConstants<double>::twoPi * 2.0 * u));
                break;
            }
            case freeze:
            {
                const double g = juce::jmin (L, juce::jmax (sr * 0.03, juce::jmin (sr * 0.09, L * 0.25)));
                if (t < g) src = m.start + t;
                else       loopFrom (m.start, g, std::fmod (t, g), true);
                break;
            }
            case shuffle:  src = m.base + juce::jmax (0.0, t); break;
            default:       outL = liveL; outR = liveR; return;
        }

        float l = readAt (0, src, now), r = readAt (1, src, now);
        if (blend > 0.0f && blendSrc >= 0.0)
        {
            l = l * (1.0f - blend) + readAt (0, blendSrc, now) * blend;
            r = r * (1.0f - blend) + readAt (1, blendSrc, now) * blend;
        }
        outL = l * env * m.gain;
        outR = r * env * m.gain;
    }

    void Mangler::onStep (double ppq, double pps, double ppqPerBar, int64_t now, int64_t bar, int step,
                          const mangle::Pattern& pat, const Gate& gate) noexcept
    {
        const int S = gate.steps;
        const double stepPpq = ppqPerBar / (double) S;

        if (bar != lastBar)
        {
            // AMOUNT: which steps fire this bar.
            fireMask = 0;
            for (int s = 0; s < S; ++s)
                if (gate.amount >= 0.999f || rng.nextFloat() < gate.amount) fireMask |= 1u << s;
            if (fillPending) { fillBar = bar; fillPending = false; }
        }

        bool inWindow = true;
        if (gate.fillEvery > 0)
            inWindow = (bar % gate.fillEvery) == gate.fillEvery - 1 && (! gate.fillHalf || step >= S / 2);
        active = gate.hold || bar == fillBar || (gate.on && inWindow);

        Map m;
        const int rs = pat.runStart (juce::jmin (step, S - 1));
        const int rl = pat.runLength (rs, S);
        const int mv = (active && (fireMask >> rs & 1u)) ? (int) pat.move[(size_t) rs] : (int) mangle::thru;
        const double samplesPerPpq = 1.0 / juce::jmax (1.0e-12, pps);
        const double runStartPpq = (double) bar * ppqPerBar + rs * stepPpq;
        m.move  = mv;
        m.start = (double) now - (ppq - runStartPpq) * samplesPerPpq;
        m.len   = rl * stepPpq * samplesPerPpq;
        m.slices = mangle::kRepeatSlices[juce::jlimit (0, 2, (int) pat.arg[(size_t) rs])];
        m.gain  = mv == mangle::thru ? 1.0f : pat.trim;
        const double barStart = (double) now - (ppq - (double) bar * ppqPerBar) * samplesPerPpq;
        const double barLen = ppqPerBar * samplesPerPpq;
        const int src = juce::jlimit (0, S - 1, (int) pat.arg[(size_t) rs]);
        m.base = barStart - barLen + src * stepPpq * samplesPerPpq;

        // Same run and move as before (the next step of a joined run): carry on.
        const bool same = m.move == cur.move && std::abs (m.start - cur.start) < 2.0 && std::abs (m.len - cur.len) < 2.0
                          && m.slices == cur.slices && std::abs (m.base - cur.base) < 2.0;
        if (! same)
        {
            old = cur;
            cur = m;
            xfPos = 0;
        }
        lastBar = bar;
        lastStep = step;
    }

    void Mangler::process (float* left, float* right, int n, const FxClock& clock,
                           const mangle::Pattern& pat, const Gate& gate) noexcept
    {
        if (ringSize <= 1 || n <= 0) return;
        const int S = juce::jlimit (1, mangle::Pattern::kSteps, gate.steps);
        const bool playing = clock.playing && clock.ppqPerSample > 0.0;
        const double ppqPerBar = clock.ppqPerBar > 0.0 ? clock.ppqPerBar : 4.0;
        mixSm.setTargetValue (juce::jlimit (0.0f, 1.0f, gate.mix));

        // FILL NEXT presses arm one bar from the next bar line.
        if (gate.fillNextCount != fillSeen) { fillSeen = gate.fillNextCount; fillPending = true; }

        // Randomize's ears: the last N whole bars, ending at the latest bar line.
        if (captureReq.load (std::memory_order_acquire) != captureSeen && playing)
        {
            captureSeen = captureReq.load();
            const double barSamples = ppqPerBar / clock.ppqPerSample;
            const double intoBar = std::fmod (clock.ppq, ppqPerBar) / clock.ppqPerSample;
            const int bars = juce::jlimit (1, 4, captureBars.load());
            const int len = (int) juce::jmin ((double) ringSize - 1.0, bars * barSamples);
            const int64_t end = written - (int64_t) intoBar;
            const int64_t begin = end - len;
            if (len > 0 && begin >= 0 && written - begin < (int64_t) ringSize)
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* d = capture.getWritePointer (ch);
                    for (int i = 0; i < len; ++i) d[i] = ring.getSample (ch, (int) ((begin + i) % ringSize));
                }
                capturedLength = len;
                capturedBpm = clock.bpm;
                capturedBarSamples = barSamples;
                captureDone.fetch_add (1, std::memory_order_release);
            }
        }

        for (int i = 0; i < n; ++i)
        {
            const float dl = left[i];
            const float dr = right != nullptr ? right[i] : dl;
            const int64_t now = written;
            const int idx = (int) (now % ringSize);
            ring.setSample (0, idx, dl);
            ring.setSample (1, idx, dr);
            ++written;

            if (playing)
            {
                const double ppq = clock.ppq + i * clock.ppqPerSample;
                const auto bar = (int64_t) std::floor (ppq / ppqPerBar + 1.0e-9);
                const double inBar = ppq - (double) bar * ppqPerBar;
                const int step = juce::jlimit (0, S - 1, (int) std::floor (inBar / (ppqPerBar / S) + 1.0e-9));
                if (step != lastStep || bar != lastBar)
                    onStep (ppq, clock.ppqPerSample, ppqPerBar, now, bar, step, pat, gate);
            }
            else if (cur.move != mangle::thru || lastStep >= 0)
            {
                // Live stopped: fade back to the input and start fresh on the next play.
                if (cur.move != mangle::thru) { old = cur; cur = {}; xfPos = 0; }
                lastStep = -1; lastBar = -1; active = false;
            }

            float wl, wr;
            value (cur, now, dl, dr, wl, wr);
            if (xfPos < xfLen)
            {
                float ol, orr;
                value (old, now, dl, dr, ol, orr);
                const float g = (float) xfPos / (float) xfLen;
                wl = ol + (wl - ol) * g;
                wr = orr + (wr - orr) * g;
                ++xfPos;
            }
            const float mx = mixSm.getNextValue();
            left[i] = dl + (wl - dl) * mx;
            if (right != nullptr) right[i] = dr + (wr - dr) * mx;
        }

        uiStep.store (playing ? lastStep : -1, std::memory_order_relaxed);
        uiActive.store (playing && active, std::memory_order_relaxed);
    }

    void Mangler::renderOffline (const mangle::Pattern& pat, int steps, const juce::AudioBuffer<float>& in, int length,
                                 double sampleRate, double bpm, double ppqPerBar, juce::AudioBuffer<float>& out)
    {
        Mangler m;
        m.prepare (sampleRate, 512);
        out.setSize (2, juce::jmax (1, length), false, true, false);
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, 0, in, juce::jmin (ch, in.getNumChannels() - 1), 0, length);
        Gate g;
        g.on = true; g.steps = steps; g.amount = 1.0f; g.mix = 1.0f;
        m.mixSm.setCurrentAndTargetValue (1.0f);
        FxClock c;
        c.bpm = bpm; c.ppqPerBar = ppqPerBar; c.playing = true;
        c.ppqPerSample = bpm / 60.0 / sampleRate;
        for (int pos = 0; pos < length; pos += 512)
        {
            const int k = juce::jmin (512, length - pos);
            c.ppq = pos * c.ppqPerSample;
            m.process (out.getWritePointer (0, pos), out.getWritePointer (1, pos), k, c, pat, g);
        }
    }
}
