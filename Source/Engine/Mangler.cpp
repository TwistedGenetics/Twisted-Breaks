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
        activeSm.reset (sr, 0.01);
        texSize = (int) std::ceil (sr * 2.0);
        texRing.setSize (2, texSize, false, true, false);
        dlySize = (int) std::ceil (sr * 4.5);
        dly.setSize (2, dlySize, false, true, false);
        dlyTime.reset (sr, 0.15);
        static const int apLen[8] = { 113, 337, 541, 797, 149, 389, 577, 863 };     // L 0-3, R 4-7
        for (int k = 0; k < 8; ++k) diff[(size_t) k].b.assign ((size_t) juce::jmax (8, (int) (apLen[k] * sr / 48000.0)), 0.0f);
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
        slotNow = 0;
        texRing.clear(); texWritten = 0; for (auto& g : grains) g = {}; grainClock = 0.0;
        dly.clear(); dlyPos = 0; dampL = dampR = 0.0f;
        for (auto& a : diff) { std::fill (a.b.begin(), a.b.end(), 0.0f); a.i = 0; }
        activeSm.setCurrentAndTargetValue (0.0f);
        uiStep.store (-1); uiActive.store (false);
    }

    // ---- TEXTURE --------------------------------------------------------------------------
    void Mangler::texture (float& l, float& r, const Gate& g, float act) noexcept
    {
        const int ti = (int) (texWritten % texSize);
        texRing.setSample (0, ti, l);
        texRing.setSample (1, ti, r);
        ++texWritten;
        if (g.texMode <= 0 || act <= 0.0f)
        {
            // Off: let any grains die away quietly.
            bool any = false;
            for (auto& gr : grains) any = any || gr.on;
            if (! any) return;
        }

        const int mode = g.texMode;
        const double lenS = juce::jlimit (8.0, sr * 0.5, (double) g.texSizeMs * 0.001 * sr * (mode == 3 ? 2.0 : 1.0));
        const float scat = juce::jlimit (0.0f, 1.0f, g.texScatter);
        if (mode > 0 && act > 0.0f)
        {
            grainClock -= 1.0;
            if (grainClock <= 0.0)
            {
                const double interval = sr / juce::jlimit (1.0, 120.0, (double) g.texDensity);
                grainClock = interval * (mode == 1 ? 1.0 : 0.5 + texRng.nextDouble() * (0.5 + scat));
                for (auto& gr : grains)
                {
                    if (gr.on) continue;
                    gr.on = true; gr.age = 0; gr.len = (int) lenS;
                    double back = lenS;                                             // TIGHT: just behind the input
                    if (mode == 2) back += texRng.nextDouble() * scat * 0.5 * sr;
                    if (mode == 3) back += texRng.nextDouble() * scat * 1.0 * sr;
                    if (mode == 4) back += texRng.nextDouble() * scat * 0.25 * sr;
                    back = juce::jmin (back, (double) texSize - lenS - 4.0);
                    gr.rate = mode == 4 ? -1.0 : 1.0;
                    if (mode == 3) gr.rate = std::pow (2.0, (texRng.nextDouble() * 2.0 - 1.0) * scat * 0.25);
                    gr.pos = (double) texWritten - back + (mode == 4 ? lenS : 0.0);
                    const float pan = (mode == 2 || mode == 3) ? (texRng.nextFloat() * 2.0f - 1.0f) * scat : 0.0f;
                    gr.gl = pan > 0.0f ? 1.0f - pan : 1.0f;
                    gr.gr = pan < 0.0f ? 1.0f + pan : 1.0f;
                    break;
                }
            }
        }

        float sl = 0.0f, sr2 = 0.0f;
        for (auto& gr : grains)
        {
            if (! gr.on) continue;
            const float w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) gr.age / (float) gr.len);
            const auto i = (int64_t) std::floor (gr.pos);
            const float f = (float) (gr.pos - (double) i);
            auto at = [&] (int ch, int64_t k) -> float
            {
                if (k < 0 || k >= texWritten || texWritten - k >= (int64_t) texSize) return 0.0f;
                return texRing.getSample (ch, (int) (k % texSize));
            };
            const float a = at (0, i) + (at (0, i + 1) - at (0, i)) * f;
            const float b = at (1, i) + (at (1, i + 1) - at (1, i)) * f;
            sl += a * w * gr.gl;
            sr2 += b * w * gr.gr;
            gr.pos += gr.rate;
            if (++gr.age >= gr.len) gr.on = false;
        }
        // Hann grains overlapping o times sum to about o / 2: bring them back to the input's level.
        const double overlap = juce::jmax (0.5, (double) g.texDensity * lenS / sr);
        const float norm = (float) (1.0 / juce::jmax (0.5, overlap * 0.5));
        const float amt = juce::jlimit (0.0f, 1.0f, g.texAmount) * act;
        l += (sl * norm - l) * amt;
        r += (sr2 * norm - r) * amt;
    }

    // ---- SPACE ----------------------------------------------------------------------------
    void Mangler::space (float inL, float inR, float& outL, float& outR, const Gate& g, double bpm) noexcept
    {
        outL = outR = 0.0f;
        dlyPos = (dlyPos + 1) % dlySize;             // the slot for this sample (read d behind it, write it last)
        if (g.spaceMode <= 0) { dly.setSample (0, dlyPos, 0.0f); dly.setSample (1, dlyPos, 0.0f); return; }
        const double want = juce::jlimit (16.0, (double) dlySize - 4.0, g.spaceBeats * 60.0 / juce::jmax (20.0, bpm) * sr);
        dlyTime.setTargetValue (want);
        const double d = dlyTime.getNextValue();
        auto tap = [&] (int ch)
        {
            double p = (double) dlyPos - d;
            while (p < 0.0) p += dlySize;
            const int i0 = (int) p;
            const float f = (float) (p - i0);
            const float a = dly.getSample (ch, i0), b = dly.getSample (ch, (i0 + 1) % dlySize);
            return a + (b - a) * f;
        };
        float yl = tap (0), yr = tap (1);
        const float fb = juce::jlimit (0.0f, 0.9f, g.spaceFb);
        float wl, wr;
        if (g.spaceMode == 2)          // PING-PONG: the input goes in on the left, the repeats cross over
        {
            wl = 0.5f * (inL + inR) + yr * fb;
            wr = yl * fb;
        }
        else if (g.spaceMode == 3)     // DIFFUSE: the repeats smear through all-passes, darkening
        {
            float dl = yl, dr = yr;
            for (int k = 0; k < 4; ++k) { dl = diff[(size_t) k].process (dl, 0.6f); dr = diff[(size_t) k + 4].process (dr, 0.6f); }
            dampL += (dl - dampL) * 0.35f;
            dampR += (dr - dampR) * 0.35f;
            yl = dampL; yr = dampR;
            wl = inL + dampL * fb;
            wr = inR + dampR * fb;
        }
        else
        {
            wl = inL + yl * fb;
            wr = inR + yr * fb;
        }
        // A soft ceiling inside the loop: it can never run away.
        wl = std::tanh (wl);
        wr = std::tanh (wr);
        if (std::abs (wl) < 1.0e-20f) wl = 0.0f;
        if (std::abs (wr) < 1.0e-20f) wr = 0.0f;
        dly.setSample (0, dlyPos, wl);
        dly.setSample (1, dlyPos, wr);
        const float amt = juce::jlimit (0.0f, 1.0f, g.spaceAmount);
        outL = yl * amt;
        outR = yr * amt;
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
                          const mangle::Slots& slots, const Gate& gate) noexcept
    {
        const int S = gate.steps;
        const double stepPpq = ppqPerBar / (double) S;

        if (bar != lastBar)
        {
            // The variation for this bar: the chosen one, or (CHAIN) the filled ones in turn on Live's bars.
            slotNow = juce::jlimit (0, mangle::kSlots - 1, gate.slot);
            if (gate.chain)
            {
                // A, B, C ... up to the last slot with anything in it (an empty slot between = a clean bar).
                int n = 0;
                for (int k = 0; k < mangle::kSlots; ++k) if (! mangle::isEmpty (slots[(size_t) k], S)) n = k + 1;
                if (n > 0) slotNow = (int) (((bar % n) + n) % n);
            }
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

        const auto& pat = slots[(size_t) slotNow];
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
                           const mangle::Slots& pat, const Gate& gate) noexcept
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
            // TEXTURE over the mangled sound while mangling; SPACE echoes it (and rings on after).
            activeSm.setTargetValue (playing && active ? 1.0f : 0.0f);
            const float act = activeSm.getNextValue();
            texture (wl, wr, gate, act);
            float sl, sr2;
            space (wl * act, wr * act, sl, sr2, gate, clock.bpm);
            const float mx = mixSm.getNextValue();
            left[i] = dl + (wl - dl) * mx + sl * mx;
            if (right != nullptr) right[i] = dr + (wr - dr) * mx + sr2 * mx;
        }

        uiSlot.store (slotNow, std::memory_order_relaxed);
        uiStep.store (playing ? lastStep : -1, std::memory_order_relaxed);
        uiActive.store (playing && active, std::memory_order_relaxed);
    }

    void Mangler::renderOffline (const mangle::Pattern& pat, int steps, const juce::AudioBuffer<float>& in, int length,
                                 double sampleRate, double bpm, double ppqPerBar, juce::AudioBuffer<float>& out)
    {
        Gate g;
        g.on = true; g.steps = steps; g.amount = 1.0f; g.mix = 1.0f;
        mangle::Slots s {};
        s[0] = pat;
        renderOffline (s, g, in, length, sampleRate, bpm, ppqPerBar, out);
    }

    void Mangler::renderOffline (const mangle::Slots& pat, const Gate& g, const juce::AudioBuffer<float>& in, int length,
                                 double sampleRate, double bpm, double ppqPerBar, juce::AudioBuffer<float>& out)
    {
        Mangler m;
        m.prepare (sampleRate, 512);
        out.setSize (2, juce::jmax (1, length), false, true, false);
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, 0, in, juce::jmin (ch, in.getNumChannels() - 1), 0, length);
        m.mixSm.setCurrentAndTargetValue (g.mix);
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
