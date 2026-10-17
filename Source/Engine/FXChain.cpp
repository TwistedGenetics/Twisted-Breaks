#include "Engine/FXChain.h"
#include <cmath>

namespace tb
{
    using namespace fx;

    namespace
    {
        constexpr double kHistorySeconds = 3.0;
        constexpr double kDelaySeconds   = 4.0;
        constexpr float  kTailFloor      = 1.0e-5f;

        bool live (const juce::SmoothedValue<float>& w) noexcept
        {
            return w.getCurrentValue() > 0.0f || w.getTargetValue() > 0.0f;
        }

        int choiceIndex (float raw, int count) noexcept { return juce::jlimit (0, count - 1, juce::roundToInt (raw)); }

        double frac (double x) noexcept { return x - std::floor (x); }
    }

    //==========================================================================
    void FXChain::History::writeSilence (int n) noexcept
    {
        int i = (int) (written % size);
        int left = juce::jmin (n, size);
        while (left > 0)
        {
            const int run = juce::jmin (left, size - i);
            buf.clear (i, run);
            i = (i + run) % size;
            left -= run;
        }
        written += n;
    }

    //==========================================================================
    void FXChain::prepare (double sampleRate, int maxSpanSamples)
    {
        sr      = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxSpan = juce::jmax (64, maxSpanSamples);

        for (auto& w : wet) w.reset (sr, 0.01);

        const juce::dsp::ProcessSpec spec { sr, (juce::uint32) maxSpan, 2 };
        const juce::dsp::StateVariableTPTFilterType types[] = { juce::dsp::StateVariableTPTFilterType::lowpass,
                                                                juce::dsp::StateVariableTPTFilterType::highpass,
                                                                juce::dsp::StateVariableTPTFilterType::bandpass };
        for (int j = 0; j < 3; ++j)
        {
            filters[(size_t) j].setType (types[j]);
            filters[(size_t) j].prepare (spec);
            cutoff[(size_t) j].reset (sr, 0.03);
            cutoff[(size_t) j].setCurrentAndTargetValue (1000.0f);
        }

        const int histFrames = (int) (kHistorySeconds * sr) + maxSpan;
        akai.prepare (sr);
        repeatL.hist.prepare (histFrames);
        reverseL.hist.prepare (histFrames);
        pitchHist.prepare (histFrames);

        delayBuf.setSize (2, (int) (kDelaySeconds * sr) + 4, false, true, false);
        delayTime.reset (sr, 0.08);
        delayTime.setCurrentAndTargetValue ((float) (0.25 * sr));

        freeverb.setSampleRate (sr);
        scratch.setSize (2, maxSpan, false, true, false);

        combBuf.setSize (2, (int) (sr / 30.0) + 8, false, true, false);
        combDelay.reset (sr, 0.03);
        combDelay.setCurrentAndTargetValue ((float) (sr / 130.81));

        reset();
    }

    void FXChain::reset() noexcept
    {
        for (auto& w : wet) w.setCurrentAndTargetValue (0.0f);
        for (auto& f : filters) f.reset();
        holdValue = {};
        ratePhase = 1.0;
        tapeLp = {};
        gateGain = 1.0f;

        akai.reset();
        for (Looper* lp : { &repeatL, &reverseL })
        {
            lp->state = Looper::State::off;
            lp->wet = lp->wetTarget = 0.0f;
            lp->want = false;
            lp->hist.buf.clear();
            lp->hist.written = 0;
        }
        pitchHist.buf.clear();
        pitchHist.written = 0;
        pitchActive = false;

        delayBuf.clear();
        delayLp = {};
        delayRinging = false;
        delaySilent  = 0;
        freeverb.reset();
        reverbTail = 0.0f;
        combBuf.clear();
        combLp = {};
        targetMask = 0;
    }

    void FXChain::setTargets (uint32_t mask, const std::array<std::array<float, 3>, 16>& params) noexcept
    {
        targetMask = mask;
        prm = params;

        for (int f = 0; f < kNumFx; ++f)
        {
            const bool on = (mask >> f) & 1u;
            switch (f)
            {
                case repeat:  repeatL.want  = on; break;
                case reverse: reverseL.want = on; break;
                default:      wet[(size_t) f].setTargetValue (on ? 1.0f : 0.0f); break;
            }
        }
    }

    float FXChain::beatsToSamples (double beats, const FxClock& c) const noexcept
    {
        return (float) (beats * 60.0 / juce::jmax (20.0, c.bpm) * sr);
    }

    double FXChain::nextBoundary (double ppq, double beats) const noexcept
    {
        return std::ceil (ppq / beats - 1.0e-7) * beats;
    }

    bool FXChain::isIdle() const noexcept
    {
        for (int f = 0; f < kNumFx; ++f)
            if (f != repeat && f != reverse && live (wet[(size_t) f]))
                return false;
        for (const Looper* lp : { &repeatL, &reverseL })
            if (lp->want || lp->state != Looper::State::off || lp->wet > 0.0f)
                return false;
        return ! pitchActive && ! delayRinging && reverbTail < kTailFloor;
    }

    uint32_t FXChain::engagedMask() const noexcept
    {
        uint32_t m = 0;
        for (int f = 0; f < kNumFx; ++f)
        {
            bool on = false;
            switch (f)
            {
                case repeat:  on = repeatL.wet > 0.0f;  break;
                case reverse: on = reverseL.wet > 0.0f; break;
                default:      on = wet[(size_t) f].getCurrentValue() > 0.0f || wet[(size_t) f].getTargetValue() > 0.0f; break;
            }
            if (on) m |= 1u << f;
        }
        return m;
    }

    uint32_t FXChain::armedMask() const noexcept
    {
        uint32_t m = 0;
        if (repeatL.want  && repeatL.wet  <= 0.0f) m |= 1u << repeat;
        if (reverseL.want && reverseL.wet <= 0.0f) m |= 1u << reverse;
        return m;
    }

    //==========================================================================
    void FXChain::advanceSilent (int n, const FxClock& c) noexcept
    {
        for (History* h : { &repeatL.hist, &reverseL.hist })
            h->writeSilence (n);
        for (int i = 0; i < n; ++i) akai.push (0.0f, 0.0f);
        freePpq = (c.playing ? c.ppq : freePpq) + n * (c.playing ? c.ppqPerSample : c.bpm / 60.0 / sr);
        for (auto& w : wet) w.skip (n);
    }

    void FXChain::recordOnly (const float* l, const float* r, int n, const FxClock& c) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            for (History* h : { &repeatL.hist, &reverseL.hist })
                h->write (l[i], r[i]);
            akai.push (l[i], r[i]);
        }
        freePpq = (c.playing ? c.ppq : freePpq) + n * (c.playing ? c.ppqPerSample : c.bpm / 60.0 / sr);
    }

    //==========================================================================
    void FXChain::process (float* L, float* R, int n, const FxClock& c) noexcept
    {
        const double ppq0 = c.playing ? c.ppq : freePpq;
        const double dppq = c.playing ? c.ppqPerSample : c.bpm / 60.0 / sr;
        float* ch[2] = { L, R };

        // ---- 1. MUTE -------------------------------------------------------------
        if (live (wet[mute]))
            for (int i = 0; i < n; ++i)
            {
                const float g = 1.0f - wet[mute].getNextValue();
                L[i] *= g; R[i] *= g;
            }

        // ---- 2-4. Filters (state-variable TPT, optional pre-drive) -----------------
        for (int j = 0; j < 3; ++j)
        {
            const int f = lpf + j;
            auto& w = wet[(size_t) f];
            if (! live (w)) continue;
            auto& flt = filters[(size_t) j];
            if (w.getCurrentValue() <= 0.0f) flt.reset();           // engaging from off: fresh state

            cutoff[(size_t) j].setTargetValue (juce::jlimit (20.0f, (float) (sr * 0.45), cutoffHz (prm[(size_t) f][0])));
            const float q = 0.6f * std::pow (16.0f, juce::jlimit (0.0f, 1.0f, prm[(size_t) f][1]));
            flt.setResonance (q);
            const float bpNorm = f == bpf ? 1.0f / q : 1.0f;          // band-pass peaks at Q: keep it at unity
            const float drv = juce::jlimit (0.0f, 1.0f, prm[(size_t) f][2]);
            const float g   = juce::Decibels::decibelsToGain (24.0f * drv);
            const float makeup = 1.0f / std::sqrt (g);

            for (int i = 0; i < n; ++i)
            {
                const float cf = cutoff[(size_t) j].getNextValue();
                if ((i & 15) == 0) flt.setCutoffFrequency (cf);
                const float m = w.getNextValue();
                for (int k = 0; k < 2; ++k)
                {
                    const float x  = ch[k][i];
                    const float xd = drv > 0.001f ? std::tanh (x * g) * makeup : x;
                    const float y  = flt.processSample (k, xd) * bpNorm;
                    ch[k][i] = x + m * (y - x);
                }
            }
        }

        // ---- 5. BIT CRUSH ------------------------------------------------------------
        if (live (wet[crush]))
        {
            const float steps = std::pow (2.0f, bits (prm[crush][0]) - 1.0f);
            const float mix   = juce::jlimit (0.0f, 1.0f, prm[crush][1]);
            for (int i = 0; i < n; ++i)
            {
                const float m = wet[crush].getNextValue();
                for (int k = 0; k < 2; ++k)
                {
                    const float x = ch[k][i];
                    const float y = std::round (x * steps) / steps;
                    ch[k][i] = x + m * mix * (y - x);
                }
            }
        }

        // ---- 6. SAMPLE RATE REDUCTION (sample and hold) --------------------------------
        if (live (wet[rate]))
        {
            if (wet[rate].getCurrentValue() <= 0.0f) ratePhase = 1.0;
            const double hz = rateHz (prm[rate][0]);
            const bool stepped = choiceIndex (prm[rate][1], 2) == 1;
            const double inc = stepped ? 1.0 / juce::jmax (1.0, std::round (sr / hz))      // whole-number divisions
                                       : juce::jmin (1.0, hz / sr);
            const float mix = juce::jlimit (0.0f, 1.0f, prm[rate][2]);
            for (int i = 0; i < n; ++i)
            {
                ratePhase += inc;
                if (ratePhase >= 1.0 - 1.0e-9)
                {
                    ratePhase -= std::floor (ratePhase + 1.0e-9);
                    holdValue = { L[i], R[i] };
                }
                const float m = wet[rate].getNextValue() * mix;
                if (m >= 1.0f) { L[i] = holdValue[0]; R[i] = holdValue[1]; }      // exact hold, no rounding noise
                else { L[i] += m * (holdValue[0] - L[i]); R[i] += m * (holdValue[1] - R[i]); }
            }
        }

        // ---- 7. DISTORTION / DRIVE -------------------------------------------------------
        if (live (wet[drive]))
        {
            const int   type = choiceIndex (prm[drive][0], 3);
            const float g    = juce::Decibels::decibelsToGain (driveDb (prm[drive][1]));
            const float mix  = juce::jlimit (0.0f, 1.0f, prm[drive][2]);
            const float bias = 0.3f, biasOut = std::tanh (bias);
            for (int i = 0; i < n; ++i)
            {
                const float m = wet[drive].getNextValue() * mix;
                for (int k = 0; k < 2; ++k)
                {
                    const float x = ch[k][i], d = x * g;
                    float y;
                    switch (type)
                    {
                        case 1:  y = juce::jlimit (-1.0f, 1.0f, d); break;                    // HARD clip
                        case 2:  y = (std::tanh (d + bias) - biasOut) / (1.0f - biasOut * 0.5f); break; // SAT: asymmetric
                        default: y = std::tanh (d); break;                                    // SOFT clip
                    }
                    ch[k][i] = x + m * (y - x);
                }
            }
        }

        // ---- 8. TAPE: saturation + high-frequency roll-off ------------------------------
        if (live (wet[tape]))
        {
            if (wet[tape].getCurrentValue() <= 0.0f) tapeLp = { L[0], R[0] };
            const float s    = juce::jlimit (0.0f, 1.0f, prm[tape][0]);
            const float coef = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * juce::jmin (toneHz (prm[tape][1]), (float) (sr * 0.45)) / (float) sr);
            for (int i = 0; i < n; ++i)
            {
                const float m = wet[tape].getNextValue();
                for (int k = 0; k < 2; ++k)
                {
                    const float x   = ch[k][i];
                    const float sat = x + s * (std::tanh (1.6f * x) * 0.85f - x);
                    tapeLp[(size_t) k] += coef * (sat - tapeLp[(size_t) k]);
                    ch[k][i] = x + m * (tapeLp[(size_t) k] - x);
                }
            }
        }

        // ---- 9. AKAIZER: Akai cyclic pitch (its input is kept even while it's off) ----------
        {
            const bool on = live (wet[akaizer]);
            if (on) akai.setParams (akaiSemis (prm[akaizer][0]), cycleMs (prm[akaizer][1]), choiceIndex (prm[akaizer][2], 2) == 1);
            for (int i = 0; i < n; ++i)
            {
                akai.push (L[i], R[i]);
                if (! on) continue;
                float wl, wr;
                akai.next (wl, wr);
                const float m = wet[akaizer].getNextValue();
                L[i] += m * (wl - L[i]);
                R[i] += m * (wr - R[i]);
            }
        }

        // ---- 10. BEAT REPEAT ---------------------------------------------------------------
        processLooper (repeat,  repeatL,  L, R, n, ppq0, dppq, c);

        // ---- 11. GATE: tempo-synced chopper with 2 ms edges -------------------------------
        if (live (wet[gate]))
        {
            const double div  = kGateBeats[choiceIndex (prm[gate][0], 3)];
            const double duty = juce::jlimit (0.05, 0.95, (double) prm[gate][1]);
            const float  step = 1.0f / (0.002f * (float) sr);          // 2 ms linear edges
            for (int i = 0; i < n; ++i)
            {
                const double ph = frac ((ppq0 + i * dppq) / div);
                gateGain = ph < duty ? juce::jmin (1.0f, gateGain + step) : juce::jmax (0.0f, gateGain - step);
                const float m = wet[gate].getNextValue();
                const float g = 1.0f - m + m * gateGain;
                L[i] *= g; R[i] *= g;
            }
        }

        // ---- 12. PITCH DROP: tape-stop dive ---------------------------------------------------
        {
            const bool want = (targetMask >> pitch) & 1u;
            if (want && ! pitchActive)
            {
                pitchActive   = true;
                pitchRead     = (double) pitchHist.written;
                pitchRate     = 1.0;
                pitchRateStep = 1.0 / juce::jmax (1.0f, beatsToSamples (kDropBeats[choiceIndex (prm[pitch][0], 5)], c));
            }
            if (pitchActive)
            {
                for (int i = 0; i < n; ++i)
                {
                    pitchHist.write (L[i], R[i]);
                    const float env = (float) juce::jmin (1.0, pitchRate * 20.0);     // fade out as it stops
                    const float yl = pitchHist.readInterp (0, pitchRead) * env;
                    const float yr = pitchHist.readInterp (1, pitchRead) * env;
                    pitchRead += pitchRate;
                    pitchRate  = juce::jmax (0.0, pitchRate - pitchRateStep);
                    const float m = wet[pitch].getNextValue();
                    L[i] += m * (yl - L[i]);
                    R[i] += m * (yr - R[i]);
                }
                if (! want && wet[pitch].getCurrentValue() <= 0.0f && ! wet[pitch].isSmoothing())
                    pitchActive = false;
            }
            else
            {
                wet[pitch].skip (n);
            }
        }

        // ---- 13. DELAY: synced throw with filtered, saturated feedback --------------------
        if (live (wet[delay]) || delayRinging)
        {
            const int   size = delayBuf.getNumSamples();
            delayTime.setTargetValue (juce::jlimit (16.0f, (float) (size - 4),
                                                    beatsToSamples (kDelayBeats[choiceIndex (prm[delay][0], 6)], c)));
            const float fb   = juce::jlimit (0.0f, 1.0f, prm[delay][1]) * 0.95f;
            const float mix  = juce::jlimit (0.0f, 1.0f, prm[delay][2]);
            const float lpc  = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 4500.0f / (float) sr);
            float peak = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                const float send = wet[delay].getNextValue();
                const float dt   = delayTime.getNextValue();
                double rp = (double) delayWrite - (double) dt;
                if (rp < 0.0) rp += size;
                const int   i0 = (int) rp;
                const float fr = (float) (rp - i0);
                const int   i1 = (i0 + 1) % size;
                for (int k = 0; k < 2; ++k)
                {
                    const float d = delayBuf.getSample (k, i0) + fr * (delayBuf.getSample (k, i1) - delayBuf.getSample (k, i0));
                    delayLp[(size_t) k] += lpc * (d - delayLp[(size_t) k]);
                    const float in = ch[k][i] * send + std::tanh (delayLp[(size_t) k] * fb);
                    delayBuf.setSample (k, delayWrite, in);
                    ch[k][i] += mix * d;
                    peak = juce::jmax (peak, std::abs (d), std::abs (in));
                }
                delayWrite = (delayWrite + 1) % size;
            }
            // Ringing until a whole delay time has gone by in silence (echoes
            // are spaced out, so a quiet moment between them isn't the end).
            delaySilent  = peak >= kTailFloor ? 0 : delaySilent + n;
            delayRinging = delaySilent <= (int) delayTime.getCurrentValue() + n;
        }

        // ---- 14. REVERB: throw into a plate-ish Freeverb --------------------------------
        if (live (wet[reverb]) || reverbTail >= kTailFloor)
        {
            juce::Reverb::Parameters rp;
            rp.roomSize   = 0.3f + 0.69f * juce::jlimit (0.0f, 1.0f, prm[reverb][0]);
            rp.damping    = juce::jlimit (0.0f, 1.0f, prm[reverb][1]);
            rp.wetLevel   = 1.0f;
            rp.dryLevel   = 0.0f;
            rp.width      = 1.0f;
            rp.freezeMode = 0.0f;
            freeverb.setParameters (rp);
            const float mix = juce::jlimit (0.0f, 1.0f, prm[reverb][2]);

            auto* sl = scratch.getWritePointer (0);
            auto* sr2 = scratch.getWritePointer (1);
            for (int i = 0; i < n; ++i)
            {
                const float send = wet[reverb].getNextValue();
                sl[i] = L[i] * send;
                sr2[i] = R[i] * send;
            }
            freeverb.processStereo (sl, sr2, n);
            float peak = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                L[i] += mix * sl[i];
                R[i] += mix * sr2[i];
                peak = juce::jmax (peak, std::abs (sl[i]), std::abs (sr2[i]));
            }
            reverbTail = peak;
        }

        // ---- 15. COMB / RESONATOR -------------------------------------------------------------
        if (live (wet[comb]))
        {
            const int size = combBuf.getNumSamples();
            combDelay.setTargetValue (juce::jlimit (2.0f, (float) (size - 2), (float) sr / combHz (prm[comb][0])));
            const float fb   = juce::jlimit (0.0f, 1.0f, prm[comb][1]) * 0.97f;
            const float a    = 1.0f - 0.85f * juce::jlimit (0.0f, 1.0f, prm[comb][2]);
            const float norm = 1.0f - 0.6f * fb;
            for (int i = 0; i < n; ++i)
            {
                const float dl = combDelay.getNextValue();
                double rp = (double) combWrite - (double) dl;
                if (rp < 0.0) rp += size;
                const int   i0 = (int) rp;
                const float fr = (float) (rp - i0);
                const int   i1 = (i0 + 1) % size;
                const float m  = wet[comb].getNextValue();
                for (int k = 0; k < 2; ++k)
                {
                    const float d = combBuf.getSample (k, i0) + fr * (combBuf.getSample (k, i1) - combBuf.getSample (k, i0));
                    combLp[(size_t) k] += a * (d - combLp[(size_t) k]);
                    const float x = ch[k][i];
                    const float y = x + fb * combLp[(size_t) k];
                    combBuf.setSample (k, combWrite, y);
                    ch[k][i] = x + m * (y * norm - x);
                }
                combWrite = (combWrite + 1) % size;
            }
        }

        // ---- 16. REVERSE -----------------------------------------------------------------------
        processReverse (L, R, n, ppq0, dppq, c);

        freePpq = ppq0 + n * dppq;
    }

    //==========================================================================
    void FXChain::processLooper (int id, Looper& lp, float* L, float* R, int n,
                                 double ppq0, double dppq, const FxClock& c) noexcept
    {
        const double size  = kSizeBeats[choiceIndex (prm[(size_t) id][0], 5)];
        const float  wStep = 1.0f / (0.003f * (float) sr);       // 3 ms crossfade
        const int    xf    = juce::jmax (8, (int) (0.003 * sr)); // loop seam crossfade
        const int    maxLen = lp.hist.size - maxSpan - xf - 8;

        for (int i = 0; i < n; ++i)
        {
            const double  ppq = ppq0 + i * dppq;
            const int64_t abs = lp.hist.written;
            lp.hist.write (L[i], R[i]);

            // Waiting for the boundary (first engage, or a SIZE change while on).
            const bool sizeChanged = lp.state == Looper::State::on && lp.want && std::abs (size - lp.sizeBeats) > 1.0e-9;
            if ((lp.want && lp.state == Looper::State::off) || sizeChanged)
            {
                lp.state = Looper::State::armed;
                lp.sizeBeats = size;
            }

            if (lp.state == Looper::State::armed)
            {
                if (! lp.want) { lp.state = lp.wet > 0.0f ? Looper::State::on : Looper::State::off; }
                else
                {
                    // On a boundary of SIZE (or within half a sample of one)?
                    const double pos = ppq / lp.sizeBeats;
                    const bool onBoundary = frac (pos) < (dppq / lp.sizeBeats) * 0.5 + 1.0e-9
                                         || frac (pos) > 1.0 - (dppq / lp.sizeBeats) * 0.5;
                    if (onBoundary)
                    {
                        const int len = juce::jlimit (16, maxLen, (int) std::lround (beatsToSamples (lp.sizeBeats, c)));
                        lp.loopLen = len;
                        lp.loopStart = abs - len; lp.playStart = abs;                                  // repeat what just played
                        lp.state = Looper::State::on;
                    }
                }
            }

            if (lp.state == Looper::State::on)
                lp.wetTarget = (lp.want && abs >= lp.playStart) ? 1.0f : (lp.want ? lp.wet : 0.0f);
            else
                lp.wetTarget = 0.0f;

            if (lp.wet != lp.wetTarget)
                lp.wet = lp.wet < lp.wetTarget ? juce::jmin (lp.wetTarget, lp.wet + wStep)
                                               : juce::jmax (lp.wetTarget, lp.wet - wStep);

            if (lp.wet > 0.0f && lp.loopLen > 0)
            {
                const int64_t t = juce::jmax ((int64_t) 0, abs - lp.playStart);
                const int k = (int) (t % lp.loopLen);
                const int64_t src = lp.loopStart + k;
                float yl = lp.hist.read (0, src), yr = lp.hist.read (1, src);
                if (k >= lp.loopLen - xf)
                {
                    // Seam: blend toward the audio just before the loop start, which
                    // leads into the loop start exactly as the wrap will.
                    const float b = (float) (k - (lp.loopLen - xf) + 1) / (float) xf;
                    const int64_t alt = lp.loopStart - (lp.loopLen - k);
                    yl += b * (lp.hist.read (0, alt) - yl);
                    yr += b * (lp.hist.read (1, alt) - yr);
                }
                L[i] += lp.wet * (yl - L[i]);
                R[i] += lp.wet * (yr - R[i]);
            }

            if (lp.state == Looper::State::on && ! lp.want && lp.wet <= 0.0f)
                lp.state = Looper::State::off;
        }
    }

    void FXChain::processReverse (float* L, float* R, int n, double ppq0, double dppq, const FxClock& c) noexcept
    {
        auto& lp = reverseL;
        const double size  = kRevBeats[choiceIndex (prm[reverse][0], 4)];
        const float  wStep = 1.0f / (0.003f * (float) sr);
        const int    edge  = juce::jmax (8, (int) (0.002 * sr));          // 2 ms raised-cosine edges
        const int    maxLen = lp.hist.size / 2 - maxSpan - 8;

        for (int i = 0; i < n; ++i)
        {
            const double  ppq = ppq0 + i * dppq;
            const int64_t abs = lp.hist.written;
            lp.hist.write (L[i], R[i]);

            const bool sizeChanged = lp.state == Looper::State::on && lp.want && std::abs (size - lp.sizeBeats) > 1.0e-9;
            if ((lp.want && lp.state == Looper::State::off) || sizeChanged)
            {
                lp.state = Looper::State::armed;
                lp.sizeBeats = size;
            }
            if (lp.state == Looper::State::armed)
            {
                if (! lp.want) lp.state = lp.wet > 0.0f ? Looper::State::on : Looper::State::off;
                else
                {
                    const double pos = ppq / lp.sizeBeats;
                    if (frac (pos) < (dppq / lp.sizeBeats) * 0.5 + 1.0e-9 || frac (pos) > 1.0 - (dppq / lp.sizeBeats) * 0.5)
                    {
                        lp.loopLen   = juce::jlimit (32, maxLen, (int) std::lround (beatsToSamples (lp.sizeBeats, c)));
                        lp.playStart = abs;
                        lp.state     = Looper::State::on;
                    }
                }
            }

            lp.wetTarget = (lp.state == Looper::State::on && lp.want) ? 1.0f : 0.0f;
            if (lp.wet != lp.wetTarget)
                lp.wet = lp.wet < lp.wetTarget ? juce::jmin (lp.wetTarget, lp.wet + wStep)
                                               : juce::jmax (lp.wetTarget, lp.wet - wStep);

            if (lp.wet > 0.0f && lp.loopLen > 0)
            {
                // Each chunk plays the PREVIOUS chunk backwards.
                const int64_t t     = juce::jmax ((int64_t) 0, abs - lp.playStart);
                const int64_t chunk = t / lp.loopLen;
                const int     k     = (int) (t % lp.loopLen);
                const int64_t src   = lp.playStart + chunk * lp.loopLen - 1 - k;
                const int     d     = juce::jmin (k, lp.loopLen - 1 - k);
                const float   env   = d >= edge ? 1.0f
                                                : 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) d / (float) edge);
                const float yl = lp.hist.read (0, src) * env, yr = lp.hist.read (1, src) * env;
                L[i] += lp.wet * (yl - L[i]);
                R[i] += lp.wet * (yr - R[i]);
            }

            if (lp.state == Looper::State::on && ! lp.want && lp.wet <= 0.0f)
                lp.state = Looper::State::off;
        }
    }
}
