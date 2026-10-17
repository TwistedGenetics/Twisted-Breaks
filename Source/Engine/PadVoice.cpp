#include "Engine/PadVoice.h"
#include "Core/Parameters.h"
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

    //==========================================================================
    //  CHARACTER: vintage sampler modes
    //==========================================================================
    const PadVoice::Character& PadVoice::character (int i) noexcept
    {
        static const Character table[kNumCharacters] = {
            { "CLEAN",   0.0,     0,  2, 0.0f },
            { "SP-1200", 26040.0, 12, 0, 0.0f },
            { "MPC60",   40000.0, 12, 1, 15000.0f },
            { "S950",    31250.0, 12, 1, 9000.0f },
            { "LO-FI",   11025.0, 8,  0, 0.0f },
        };
        return table[juce::jlimit (0, kNumCharacters - 1, i)];
    }

    float PadVoice::readCharacter (const Character& c, const float* d, int n, double pos, double sourceRate) noexcept
    {
        if (c.rate <= 0.0 && c.bits == 0) return hermite (d, n, pos);

        auto linearAt = [d, n] (double p)
        {
            const int i0 = juce::jlimit (0, n - 1, (int) std::floor (p));
            const int i1 = juce::jmin (n - 1, i0 + 1);
            const float f = (float) (p - std::floor (p));
            return d[i0] + (d[i1] - d[i0]) * f;
        };

        float v;
        if (c.rate > 0.0)
        {
            // The machine's own sample grid: index k sits at source position k * step.
            const double step = sourceRate / c.rate;
            const double k = pos / step;
            const double k0 = std::floor (k);
            const float s0 = linearAt (k0 * step);
            if (c.interp == 0) v = s0;                                    // drop-sample: hold each stored sample
            else               v = s0 + (linearAt ((k0 + 1.0) * step) - s0) * (float) (k - k0);
        }
        else
        {
            v = c.interp == 2 ? hermite (d, n, pos) : linearAt (pos);
        }

        if (c.bits > 0)
        {
            const float q = (float) (1 << (c.bits - 1));
            v = juce::jlimit (-1.0f, 1.0f - 1.0f / q, std::round (v * q) / q);
        }
        return v;
    }

    void PadVoice::setCharacter (int c) noexcept
    {
        c = juce::jlimit (0, kNumCharacters - 1, c);
        if (c != characterIndex) { lp1L = lp2L = lp1R = lp2R = 0.0f; }
        characterIndex = c;
        const float hz = character (c).lowpassHz;
        lpCoef = hz > 0.0f ? (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * hz / hostRate)) : 0.0f;
    }

    float PadVoice::read (const float* d, int n, double pos) const noexcept
    {
        if (characterIndex == 0) return hermite (d, n, pos);
        return readCharacter (character (characterIndex), d, n, pos, buffer != nullptr ? buffer->sourceSampleRate : hostRate);
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

    double PadVoice::gridPosition (const Settings& s, double ppq) noexcept
    {
        const double len = (double) (s.regionEnd - s.regionStart);
        if (s.syncPeriodBeats <= 0.0 || len <= 0.0) return (double) (s.reverse ? s.regionEnd - 1 : s.regionStart);
        double phase = ppq / s.syncPeriodBeats;
        phase -= std::floor (phase);
        return s.reverse ? (double) s.regionEnd - 1.0 - phase * len
                         : (double) s.regionStart + phase * len;
    }

    double PadVoice::phaseLockedPosition (double ppq) const noexcept
    {
        Settings s;
        s.regionStart = regionStart; s.regionEnd = regionEnd;
        s.reverse = reverse; s.syncPeriodBeats = syncPeriod;
        double p = gridPosition (s, ppq) + phaseOffset;

        // Keep it inside the region.
        const double lo = (double) regionStart, len = (double) (regionEnd - regionStart);
        if (len > 0.0)
        {
            p = std::fmod (p - lo, len);
            if (p < 0.0) p += len;
            p += lo;
        }
        return p;
    }

    void PadVoice::start (const SampleBuffer::Ptr& newBuffer, const Settings& s, float level,
                          double startPpq, bool hostPlaying, double startAt) noexcept
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
        characterIndex = -1;
        setCharacter (s.character);
        lp1L = lp2L = lp1R = lp2R = 0.0f;
        pressAmount = 0.0f; pressCoef = 0.0f; driveMix = 0.0f; pitchMul = 1.0; stuttering = false;
        offGrid = false; snapCountdown = 0;
        velocityGain = juce::jlimit (0.0f, 1.0f, level);
        releaseGain  = 1.0f;
        releaseStep  = 1.0f / juce::jmax (1.0f, (float) (hostRate * 0.005));

        position    = reverse ? (double) (regionEnd - 1) : (double) regionStart;
        phaseOffset = 0.0;

        // A synced loop enters at the point the host's playhead says it should
        // be at, so it lands on the grid even when triggered mid-bar.
        const bool lockToHost = syncPeriod > 0.0 && hostPlaying;
        if (lockToHost) position = phaseLockedPosition (startPpq);

        // SWAP hand-off: start exactly here. A synced loop remembers how far
        // that is from its grid position, so syncTo() keeps it there.
        const bool forced = startAt >= 0.0;
        if (forced)
        {
            const double grid = position;
            position = juce::jlimit ((double) regionStart, (double) juce::jmax (regionStart, regionEnd - 1), startAt);
            if (lockToHost)
            {
                const double len = (double) (regionEnd - regionStart);
                double off = position - grid;
                if (off >  len * 0.5) off -= len;
                if (off < -len * 0.5) off += len;
                phaseOffset = off;
            }
        }

        // Starting exactly at the top of the file forwards keeps the attack
        // sample-exact; anywhere else gets a tiny fade so it can't click.
        const bool needsFade = reverse || regionStart > 0 || lockToHost || (forced && position > 0.0);
        // Short (0.5 ms): slices start just before their attack, so the fade
        // is over before the hit and never softens it.
        fadeIn     = needsFade ? 0.0f : 1.0f;
        fadeInStep = 1.0f / juce::jmax (4.0f, (float) (hostRate * 0.0005));

        increment.setCurrentAndTargetValue (s.increment);
        gainL.setCurrentAndTargetValue (s.gainLeft);
        gainR.setCurrentAndTargetValue (s.gainRight);

        // A12 STRETCH: heads start at the play head.
        for (auto& h : heads) h = {};
        stretchMode = (! looping && ! reverse && ! forced) ? s.stretch : 0;
        readInc     = s.readInc;
        grainLen    = juce::jmax (32, s.grainSamples);
        gapMode     = s.gapMode;
        beats       = s.beatSlices;
        grainClock  = 0;
        gapFading = stretchEnding = false;
        beatHead = -1;
        if (stretchMode != 0)
        {
            const int fin = needsFade ? 16 : 1;
            if (stretchMode == 1)
            {
                beatIdx = beats.isEmpty() ? 0 : beats.sliceAt ((int) position);
                const int nxt = beatIdx + 1;
                sliceEnd = (! beats.isEmpty() && nxt < beats.count) ? (double) beats.starts[(size_t) nxt] : (double) regionEnd;
                beatHead = spawn (position, fin);
            }
            else if (stretchMode == 2) spawn (position, 0, true, grainLen);
            else                       spawn (position, fin);
            fadeIn = 1.0f;                        // the heads fade themselves
        }
    }

    int PadVoice::spawn (double pos, int fadeInSamples, bool hann, int len, double dir) noexcept
    {
        int k = -1;
        for (int i = 0; i < (int) heads.size(); ++i) if (! heads[(size_t) i].on) { k = i; break; }
        if (k < 0)
        {
            // All busy: take the quietest.
            k = 0;
            for (int i = 1; i < (int) heads.size(); ++i) if (heads[(size_t) i].g < heads[(size_t) k].g) k = i;
        }
        auto& h = heads[(size_t) k];
        h.on = true; h.pos = pos; h.dir = dir; h.hann = hann; h.age = 0; h.len = juce::jmax (1, len);
        h.g = fadeInSamples <= 1 ? 1.0f : 0.0f;
        h.gstep = fadeInSamples <= 1 ? 0.0f : 1.0f / (float) fadeInSamples;
        return k;
    }

    void PadVoice::fadeAll (int samples) noexcept
    {
        for (auto& h : heads)
            if (h.on && ! h.hann) h.gstep = -juce::jmax (h.g, 1.0e-3f) / (float) juce::jmax (1, samples);
    }

    double PadVoice::bestCycleStart (double around, int fadeLen) const noexcept
    {
        // AKAI REVISED: the start near `around` whose audio best matches what the
        // fading cycle plays next (so the splice joins in phase).
        int cur = -1;
        for (int i = 0; i < (int) heads.size(); ++i) if (heads[(size_t) i].on && heads[(size_t) i].gstep >= 0.0f) cur = i;
        if (cur < 0 || buffer == nullptr) return around;
        const auto& h = heads[(size_t) cur];
        const float* d = buffer->audio.getReadPointer (0);
        const int n = buffer->getNumSamples();
        const int W = juce::jlimit (16, 256, fadeLen);
        const int range = juce::jmax (8, grainLen / 4);
        auto at = [d, n] (double p) { const int i = juce::jlimit (0, n - 1, (int) p); return d[i]; };
        double best = around, bestScore = -1.0e30;
        for (int off = -range; off <= range; off += 3)
        {
            const double c = around + off * readInc;
            double score = 0.0;
            for (int k = 0; k < W; k += 4)
                score += (double) at (h.pos + k * readInc) * (double) at (c + k * readInc);
            if (score > bestScore) { bestScore = score; best = c; }
        }
        return juce::jlimit (0.0, (double) (n - 1), best);
    }

    void PadVoice::renderStretched (const float* srcL, const float* srcR, bool stereo, int len, float& l, float& r) noexcept
    {
        // Schedule: the play head (position) runs at Live's tempo; heads read at the sound's own pitch.
        if (! stretchEnding && position >= (double) regionEnd)
        {
            stretchEnding = true;
            fadeAll ((int) edgeFadeSamples);
            for (auto& h : heads) if (h.hann) { h.len = juce::jmin (h.len, h.age + (int) edgeFadeSamples); }
        }
        if (! stretchEnding)
        {
            if (stretchMode == 1)
            {
                // BEATS: each slice starts on its own spot on Live's grid.
                if (position >= sliceEnd && ! beats.isEmpty() && beatIdx + 1 < beats.count)
                {
                    ++beatIdx;
                    const double s0 = (double) beats.starts[(size_t) beatIdx];
                    const int nxt = beatIdx + 1;
                    sliceEnd = nxt < beats.count ? (double) beats.starts[(size_t) nxt] : (double) regionEnd;
                    fadeAll (64);
                    beatHead = spawn (s0 + (position - s0) * 0.0, 16);
                    gapFading = false;
                }
                else if (beatHead >= 0 && heads[(size_t) beatHead].on)
                {
                    // The slice ran out before its next grid spot: fill the gap.
                    auto& h = heads[(size_t) beatHead];
                    const double segStart = (double) (beatIdx < beats.count && ! beats.isEmpty() ? beats.starts[(size_t) beatIdx] : regionStart);
                    const double seg = juce::jmin ((sliceEnd - segStart) / 3.0, (buffer != nullptr ? buffer->sourceSampleRate : hostRate) * 0.04);
                    if (h.pos >= sliceEnd && h.dir > 0.0)
                    {
                        if (gapMode == 0 && ! gapFading) { fadeAll (64); gapFading = true; }
                        else if (gapMode == 1)          { fadeAll (64); beatHead = spawn (sliceEnd - seg, 64); }
                        else if (gapMode == 2)          h.dir = -1.0;
                    }
                    else if (gapMode == 2 && h.dir < 0.0 && h.pos <= sliceEnd - seg) h.dir = 1.0;
                }
            }
            else if (stretchMode == 2)
            {
                // TEXTURE: Hann grains every half grain, from the play head (a little scatter).
                if (++grainClock >= grainLen / 2)
                {
                    grainClock = 0;
                    jitterSeed = jitterSeed * 1664525u + 1013904223u;
                    const double jitter = ((double) (jitterSeed >> 8) / 16777216.0 - 0.5) * 0.1 * grainLen * readInc;
                    spawn (juce::jmax ((double) regionStart, position + jitter), 0, true, grainLen);
                }
            }
            else
            {
                // AKAI: a new cycle every CYCLE from the play head. CLASSIC splices almost hard
                // (the buzz at the cycle rate is the sound); REVISED crossfades in phase.
                if (++grainClock >= grainLen)
                {
                    grainClock = 0;
                    const bool revised = stretchMode == 4;
                    const int x = revised ? juce::jmax (16, (int) (grainLen * 0.35)) : juce::jmin (24, grainLen / 8);
                    const double startAt = revised ? bestCycleStart (position, x) : position;
                    fadeAll (x);
                    spawn (startAt, x);
                }
            }
        }

        l = r = 0.0f;
        bool any = false;
        for (auto& h : heads)
        {
            if (! h.on) continue;
            any = true;
            float g;
            if (h.hann)
            {
                const float t = (float) h.age / (float) h.len;
                g = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * t);
                if (++h.age >= h.len) h.on = false;
            }
            else
            {
                g = h.g;
                h.g += h.gstep;
                if (h.g >= 1.0f) { h.g = 1.0f; h.gstep = 0.0f; }
                if (h.gstep < 0.0f && h.g <= 0.0f) h.on = false;
            }
            const float a = read (srcL, len, h.pos);
            l += a * g;
            r += (stereo ? read (srcR, len, h.pos) : a) * g;
            h.pos += readInc * pitchMul * h.dir;
            if (h.pos >= (double) len - 1.0) h.on = false;
        }
        if (! any && stretchEnding) kill();
    }

    void PadVoice::update (const Settings& s) noexcept
    {
        if (! active) return;
        setCharacter (s.character);
        increment.setTargetValue (s.increment);
        if (stretchMode != 0 && s.stretch != 0) readInc = s.readInc;
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

    void PadVoice::setPressure (float amount, int target, double sixteenthSamples) noexcept
    {
        amount = juce::jlimit (0.0f, 1.0f, amount);
        if (target != pressTarget)
        {
            pressTarget = target;          // (the filter state tracks the input while open: no reset needed)
            stuttering = false;
        }
        if (target == params::pressOff) amount = 0.0f;
        pressAmount = amount;

        // FILTER: cutoff 18 kHz (no pressure) .. 300 Hz (full).
        if (target == params::pressFilter && amount > 0.0f)
        {
            const double fc = 18000.0 * std::pow (300.0 / 18000.0, (double) amount);
            pressCoef = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * juce::jmin (fc, hostRate * 0.45) / hostRate));
        }
        else pressCoef = 0.0f;

        // DRIVE: up to x16 into tanh, scaled so a -6 dBFS peak stays where it was.
        driveMix  = target == params::pressDrive ? juce::jmin (1.0f, amount * 4.0f) : 0.0f;
        driveGain = 1.0f + 15.0f * amount;
        driveNorm = 0.5f / std::tanh (driveGain * 0.5f);

        // PITCH: down to an octave.
        pitchMul = target == params::pressPitch ? std::pow (2.0, -(double) amount) : 1.0;

        // STUTTER: from 10 % pressure, from where the playhead is right then.
        if (target == params::pressStutter && amount > 0.1f)
        {
            const double div = amount > 0.85f ? 0.25 : (amount > 0.5f ? 0.5 : 1.0);
            stutterLen = juce::jmax (64, (int) std::lround (sixteenthSamples * div));
            if (! stuttering) { stuttering = true; stutterWrapped = false; stutterAnchor = position; stutterCount = 0; }
        }
        else if (amount < 0.05f || target != params::pressStutter) stuttering = false;
        if (stuttering || pitchMul < 0.9999) offGrid = true;
    }

    void PadVoice::syncTo (double ppq) noexcept
    {
        if (! active || ! looping || syncPeriod <= 0.0) return;
        if (stuttering || pitchMul < 0.9999) return;     // let off the grid while pressed (A7)

        const double len     = (double) (regionEnd - regionStart);
        const double desired = phaseLockedPosition (ppq);
        double err = desired - position;
        if (err >  len * 0.5) err -= len;
        if (err < -len * 0.5) err += len;

        // The increment is exact, so normally the error is a fraction of a
        // sample. Only a tempo change, a loop-brace wrap or a relocate moves
        // it by more — then snap back onto the grid.
        if (std::abs (err) > 2.0)
        {
            if (offGrid)
            {
                // Back from a stutter / bend: fade out over 32 samples, jump, fade in (no click).
                snapOffset = err;
                snapCountdown = kSnapFade;
            }
            else position = desired;
        }
        offGrid = false;
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
            const double inc = increment.getNextValue() * pitchMul;

            // A7 STUTTER: jump back to where the press bit, with 32-sample edges.
            float stutterEnv = 1.0f;
            if (stuttering)
            {
                if (stutterCount >= stutterLen) { position = stutterAnchor; stutterCount = 0; stutterWrapped = true; }
                const int edge = juce::jmin (32, stutterLen / 4);
                const float out = (float) (stutterLen - stutterCount) / (float) edge;
                const float in  = stutterWrapped ? (float) (stutterCount + 1) / (float) edge : 1.0f;   // the first pass is already playing
                stutterEnv = juce::jmin (1.0f, in, out);
                ++stutterCount;
            }
            if (snapCountdown > 0)
            {
                stutterEnv *= (float) snapCountdown / (float) kSnapFade;
                if (--snapCountdown == 0)
                {
                    position += snapOffset;      // back on the grid (the loop wrap below keeps it in range)
                    fadeIn = 0.0f;
                }
            }
            float env = fadeIn * velocityGain;
            if (fadeIn < 1.0f) fadeIn = juce::jmin (1.0f, fadeIn + fadeInStep);

            float l, r;

            if (looping && span >= 2.0)
            {
                // Wrap first (handles big increments and live region edits).
                while (position >= (double) hi) position -= span;
                while (position <  (double) lo) position += span;

                l = read (srcL, len, position);
                r = stereo ? read (srcR, len, position) : l;

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
                        const float al = read (srcL, len, alt);
                        const float ar = stereo ? read (srcR, len, alt) : al;
                        l = l + (al - l) * t;
                        r = r + (ar - r) * t;
                    }
                }
            }
            else if (stretchMode != 0)
            {
                renderStretched (srcL, srcR, stereo, len, l, r);
                if (! active) break;
            }
            else
            {
                // One-shot: distance to the far edge, in output samples.
                const double remaining = reverse ? (position - (double) lo + 1.0) / inc
                                                 : ((double) hi - position) / inc;
                if (remaining <= 0.0) { kill(); break; }
                if (remaining < (double) edgeFadeSamples) env *= (float) (remaining / (double) edgeFadeSamples);

                l = read (srcL, len, position);
                r = stereo ? read (srcR, len, position) : l;
            }

            // The machine's output filter (2-pole).
            if (lpCoef > 0.0f)
            {
                lp1L += lpCoef * (l - lp1L);  lp2L += lpCoef * (lp1L - lp2L);  l = lp2L;
                lp1R += lpCoef * (r - lp1R);  lp2R += lpCoef * (lp1R - lp2R);  r = lp2R;
            }

            // A7 pressure: FILTER / DRIVE.
            if (pressCoef > 0.0f)
            {
                pf1L += pressCoef * (l - pf1L);  pf2L += pressCoef * (pf1L - pf2L);  l = pf2L;
                pf1R += pressCoef * (r - pf1R);  pf2R += pressCoef * (pf1R - pf2R);  r = pf2R;
            }
            else
            {
                pf1L = pf2L = l;  pf1R = pf2R = r;     // ready to close without a jump
            }
            if (driveMix > 0.0f)
            {
                l += driveMix * (std::tanh (driveGain * l) * driveNorm - l);
                r += driveMix * (std::tanh (driveGain * r) * driveNorm - r);
            }
            env *= stutterEnv;

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
