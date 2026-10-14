#include "Engine/AudioEngine.h"
#include "Core/Parameters.h"
#include "Core/FxDefs.h"
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tb
{
    namespace
    {
        constexpr int kMinRegion = 32;   // source samples
    }

    AudioEngine::AudioEngine (EngineBridge& b) : bridge (b)
    {
        for (int p = 0; p < kNumPads; ++p)
        {
            padNote[(size_t) p].store (kDefaultFirstNote + p);
            bridge.padPlayhead[(size_t) p].store (-1.0f);
        }
        swapPlaying.fill (-1);
    }

    AudioEngine::~AudioEngine()
    {
        drainSampleQueue();
    }

    void AudioEngine::prepare (double newSampleRate, int maxBlockSize)
    {
        sampleRate = newSampleRate;

        for (auto& padVoices : voices)
            for (auto& v : padVoices)
                v.prepare (sampleRate);

        master.prepare (sampleRate, maxBlockSize);
        for (auto& g : groups)
            g.prepare (sampleRate, juce::jmax (512, maxBlockSize));

        // Scope shows roughly the same time window at any sample rate.
        scopeDecimation = juce::jmax (1, (int) std::round (sampleRate / 5512.5));
        scopeScratch.assign ((size_t) (maxBlockSize / scopeDecimation + 2), 0.0f);

        reset();
    }

    void AudioEngine::reset() noexcept
    {
        for (auto& padVoices : voices)
            for (auto& v : padVoices)
                v.kill();

        currentVoice.fill (0);
        seqSkipOnce.fill (std::numeric_limits<int64_t>::min());
        swapPlaying.fill (-1);
        pendingSwap.fill ({});
        padPeak.fill (0.0f);
        padFlash.fill (false);
        master.reset();
        for (auto& g : groups) g.reset();
        groupPeak.fill (0.0f);
        scopeCounter = 0;
    }

    //==========================================================================
    bool AudioEngine::postCommand (const PadCommand& cmd) noexcept
    {
        return padCommands.push (cmd);
    }

    void AudioEngine::drainSampleQueue() noexcept
    {
        // Not the audio thread (audio has stopped), so freeing here is fine.
        PadCommand cmd;
        while (padCommands.pop (cmd))
            if (cmd.buffer != nullptr && cmd.buffer->decReferenceCountWithoutDeleting())
                delete cmd.buffer;
    }

    void AudioEngine::applyPadCommands() noexcept
    {
        PadCommand cmd;
        while (padCommands.pop (cmd))
        {
            if (juce::isPositiveAndBelow (cmd.pad, kNumPads))
            {
                if (cmd.setSlices) ownSlices[(size_t) cmd.pad] = cmd.slices;     // plain copy, no allocation
                if (cmd.setSample) ownSample[(size_t) cmd.pad] = cmd.buffer;     // +1
                refreshPadSource (cmd.pad);
            }
            else if (juce::isPositiveAndBelow (cmd.pad - kBankCommandBase, kBankSlots))
            {
                const int slot = cmd.pad - kBankCommandBase;
                if (cmd.setSlices) bankSlices[(size_t) slot] = cmd.slices;
                if (cmd.setSample) bankSample[(size_t) slot] = cmd.buffer;
                if (slot == bankActive)
                    for (int p = 0; p < kNumPads; ++p) if (padFollowBank[(size_t) p]) refreshPadSource (p);
            }

            if (cmd.buffer != nullptr)
                cmd.buffer->decReferenceCountWithoutDeleting();       // adopt the sender's +1
        }
    }

    void AudioEngine::refreshPadSource (int pad) noexcept
    {
        const bool bank = padFollowBank[(size_t) pad] && bankSample[(size_t) bankActive] != nullptr;
        const auto& buf = bank ? bankSample[(size_t) bankActive] : ownSample[(size_t) pad];
        padSlices[(size_t) pad] = bank ? bankSlices[(size_t) bankActive] : ownSlices[(size_t) pad];
        if (padSample[(size_t) pad].get() != buf.get())
        {
            // Old voices fade out still holding the old buffer (kept alive by
            // their own reference); new triggers use the new one. A running
            // slice sequence carries on with the new break.
            for (auto& v : voices[(size_t) pad])
                v.release();
            padSample[(size_t) pad] = buf;
        }
    }

    void AudioEngine::switchBank (int slot) noexcept
    {
        bankActive = juce::jlimit (0, kBankSlots - 1, slot);
        bankActiveNow.store (bankActive, std::memory_order_relaxed);
        for (int p = 0; p < kNumPads; ++p)
            if (padFollowBank[(size_t) p]) refreshPadSource (p);
        if (currentParams != nullptr) computeVoiceSettings (*currentParams);   // regions follow the new break's length
    }

    void AudioEngine::applySnapshotPads (const std::array<SampleBuffer*, kNumPads>& buffers,
                                         const std::array<SliceTable, kNumPads>& slices) noexcept
    {
        for (int p = 0; p < kNumPads; ++p)
        {
            ownSample[(size_t) p] = buffers[(size_t) p];        // the frame keeps its own reference
            ownSlices[(size_t) p] = slices[(size_t) p];
            refreshPadSource (p);
        }
    }

    //==========================================================================
    void AudioEngine::setPadNote (int pad, int note) noexcept
    {
        if (! juce::isPositiveAndBelow (pad, kNumPads)) return;
        note = juce::jlimit (-1, 127, note);
        const int old = padNote[(size_t) pad].load();
        if (old == note) return;

        // One note -> one pad: whoever had this note gets this pad's old one.
        if (note >= 0)
            for (int p = 0; p < kNumPads; ++p)
                if (p != pad && padNote[(size_t) p].load() == note)
                    padNote[(size_t) p].store (old);

        padNote[(size_t) pad].store (note);
        noteMapVersion.fetch_add (1);
    }

    int AudioEngine::getPadNote (int pad) const noexcept
    {
        return juce::isPositiveAndBelow (pad, kNumPads) ? padNote[(size_t) pad].load() : -1;
    }

    int AudioEngine::padForNote (int note) const noexcept
    {
        for (int p = 0; p < kNumPads; ++p)
            if (padNote[(size_t) p].load (std::memory_order_relaxed) == note)
                return p;
        return -1;
    }

    //==========================================================================
    void AudioEngine::computeVoiceSettings (const BlockParams& bp) noexcept
    {
        bool anySolo = false;
        for (const auto& pp : bp.pads) anySolo = anySolo || pp.solo;
        currentParams = &bp;
        bankWanted = juce::jlimit (0, kBankSlots - 1, bp.bankActive);
        bankAtNow  = juce::jlimit (0, 2, bp.bankAt);

        // Group buses: level, pan, mute / solo (solo is between groups).
        bool anyGroupSolo = false;
        for (const auto& gp : bp.groups) anyGroupSolo = anyGroupSolo || gp.solo;
        for (int g = 0; g < kNumGroups; ++g)
        {
            const auto& gp = bp.groups[(size_t) g];
            groups[(size_t) g].setParams (gp.levelDb, gp.pan, ! gp.mute && (! anyGroupSolo || gp.solo), gp.bypass);

            uint32_t mask = 0;
            for (int f = 0; f < fx::kNumFx; ++f)
                if (gp.fxOn[(size_t) f]) mask |= 1u << f;
            fxParamMask[(size_t) g] = mask;
            fxParams[(size_t) g]    = gp.fxParam;
            // On = parameter (mouse / automation) XOR a MIDI latch, or held from MIDI.
            applyFx (g);
        }

        for (int p = 0; p < kNumPads; ++p)
        {
            const auto& pp = bp.pads[(size_t) p];
            padMode[(size_t) p]    = pp.mode;
            padChoke[(size_t) p]   = pp.choke;
            padVelSens[(size_t) p] = juce::jlimit (0.0f, 1.0f, pp.velSens);
            padFixVel[(size_t) p]  = pp.fixVel;
            padPitchNow[(size_t) p] = pp.pitch;
            if (pp.followBank != padFollowBank[(size_t) p])
            {
                padFollowBank[(size_t) p] = pp.followBank;
                refreshPadSource (p);
            }
            padSliceSlot[(size_t) p] = juce::jlimit (0, SliceSeqData::kSlots - 1, pp.sliceSlot);
            if (padSliceSeq[(size_t) p] && ! pp.sliceSeq && sliceRun[(size_t) p].running)
                stopSliceSeq (p);                     // SLICE SEQ switched off while it played
            padSliceSeq[(size_t) p] = pp.sliceSeq;
            padSwap[(size_t) p]    = juce::jlimit (0, kSwapGroups, pp.swap);
            padSwapAt[(size_t) p]  = juce::jlimit (0, 3, pp.swapAt);
            padGroup[(size_t) p]   = juce::jlimit (0, kNumGroups - 1, pp.group);

            auto& s = padSettings[(size_t) p];
            const auto* buf = padSample[(size_t) p].get();
            if (buf == nullptr) continue;

            const int len = buf->getNumSamples();

            int s0, s1;
            const auto& slices = padSlices[(size_t) p];
            if (pp.mode == params::modeSlice && ! slices.isEmpty())
            {
                // SLICE: the chosen slice is the region (START / END don't apply).
                const auto r = slices.region (pp.slice - 1, len);
                s0 = r.getStart();
                s1 = r.getEnd();
            }
            else
            {
                // Region from START / END. If automation crosses them, use the span between.
                float a = juce::jlimit (0.0f, 1.0f, pp.start);
                float b = juce::jlimit (0.0f, 1.0f, pp.end);
                if (b < a) std::swap (a, b);
                s0 = (int) std::lround ((double) a * len);
                s1 = (int) std::lround ((double) b * len);
            }
            if (s1 - s0 < kMinRegion)
            {
                s1 = juce::jmin (len, s0 + kMinRegion);
                s0 = juce::jmax (0, s1 - kMinRegion);
            }
            s.regionStart = s0;
            s.regionEnd   = s1;
            s.reverse     = pp.reverse;
            s.loop        = pp.mode == params::modeLoop;

            const double rate     = (double) params::loopRates[juce::jlimit (0, 3, pp.rate)];
            const double fileRate = buf->sourceSampleRate / sampleRate;

            if (s.loop && pp.sync)
            {
                // Tempo-synced loop: the region spans N beats (from its own
                // length), played at the host tempo x rate. Pitch follows speed.
                const double beats  = PadVoice::loopBeatsFor ((double) (s1 - s0) / buf->sourceSampleRate);
                s.syncPeriodBeats   = beats / rate;
                const double bpm    = transportNow != nullptr ? transportNow->bpm : 120.0;
                s.increment         = (double) (s1 - s0) * (bpm / 60.0) / (s.syncPeriodBeats * sampleRate);
            }
            else
            {
                // Free-running: file-rate compensation x semitones (x loop rate).
                s.syncPeriodBeats = 0.0;
                s.increment = fileRate * std::pow (2.0, (double) pp.pitch / 12.0) * (s.loop ? rate : 1.0);
            }

            // Gain + balance-style pan (centre = unity on both sides); mute / solo.
            const bool audible = ! pp.mute && (! anySolo || pp.solo);
            const float g = (! audible || pp.gainDb <= params::padGainMinDb + 0.01f)
                              ? 0.0f : juce::Decibels::decibelsToGain (pp.gainDb);
            GroupBus::panGains (g, pp.pan, s.gainLeft, s.gainRight);

            for (int i = 0; i < kVoicesPerPad; ++i)
            {
                auto& v = voices[(size_t) p][(size_t) i];
                if (v.isActive() && v.getBuffer() == buf)
                {
                    // Slice hits and tweaked steps keep their own region / pitch.
                    if (padSliceSeq[(size_t) p] || voiceCustom[(size_t) p][(size_t) i]) v.updateGains (s);
                    else                                                                 v.update (s);
                }
            }
        }
    }

    void AudioEngine::chokeOthers (int pad, int group) noexcept
    {
        if (group <= 0) return;
        for (int p = 0; p < kNumPads; ++p)
            if (p != pad && padChoke[(size_t) p] == group)
            {
                if (sliceRun[(size_t) p].running) stopSliceSeq (p);
                for (auto& v : voices[(size_t) p])
                    v.release (3.0f);           // very short, still click-free
            }
    }

    float AudioEngine::levelFor (int pad, float velocity) const noexcept
    {
        // Velocity sensitivity: 0 = every hit at full level, 1 = level follows velocity.
        // FIXED VEL: always full level, whatever the sensitivity.
        const float sens = padFixVel[(size_t) pad] ? 0.0f : padVelSens[(size_t) pad];
        return 1.0f - sens + sens * velocity;
    }

    void AudioEngine::startPad (int pad, float level, int sampleOffset, double startAt) noexcept
    {
        if (padSliceSeq[(size_t) pad]) startSliceSeq (pad, sampleOffset);
        else                           startVoice (pad, level, sampleOffset, startAt);
    }

    void AudioEngine::triggerPad (int pad, float velocity, int sampleOffset, bool fromSequencer, const SeqStep* step) noexcept
    {
        if (! juce::isPositiveAndBelow (pad, kNumPads)) return;

        bridge.padTriggerCount[(size_t) pad].fetch_add (1, std::memory_order_relaxed);
        padFlash[(size_t) pad] = true;     // lamp blinks even on an empty pad

        chokeOthers (pad, padChoke[(size_t) pad]);

        const float level = levelFor (pad, velocity);

        // ---- SWAP group: queue a hand-off instead of starting now.
        const int group = padSwap[(size_t) pad];
        if (group > 0)
        {
            auto& pending = pendingSwap[(size_t) group];
            const int owner = swapOwner (group);

            if (owner == pad)
            {
                // The playing pad again: cancel a queued swap, else stop at the next point.
                if (pending.active && pending.pad >= 0) { pending.active = false; return; }
                pending = { true, -1, level, samplesUntilSwap (owner, padSwapAt[(size_t) pad], sampleOffset) };
            }
            else if (owner >= 0)
            {
                if (padSample[(size_t) pad] == nullptr) return;
                pending = { true, pad, level, samplesUntilSwap (owner, padSwapAt[(size_t) pad], sampleOffset) };
            }
            else
            {
                // Nothing playing in the group: this pad starts right away.
                if (padSample[(size_t) pad] == nullptr) return;
                pending = {};
                swapPlaying[(size_t) group] = pad;
                startPad (pad, level, sampleOffset);
                return;
            }

            if (pending.remaining <= 0) fireSwap (group, sampleOffset);
            return;
        }

        if (padSample[(size_t) pad] == nullptr) return;

        // SLICE SEQ: a hit starts the pad's slice sequence, another stops it
        // (the main sequencer only ever starts it).
        if (padSliceSeq[(size_t) pad])
        {
            if (! sliceRun[(size_t) pad].running) startSliceSeq (pad, sampleOffset);
            else if (! fromSequencer)             stopSliceSeq (pad);
            return;
        }

        // A sequencer step with its own pitch / reverse / slice.
        if (step != nullptr && step->hasSoundTweak())
        {
            const auto s = tweakedSettings (pad, *step);
            startVoice (pad, level, sampleOffset, -1.0, &s);
            return;
        }
        startVoice (pad, level, sampleOffset);
    }

    PadVoice::Settings AudioEngine::tweakedSettings (int pad, const SeqStep& st) const noexcept
    {
        PadVoice::Settings s = padSettings[(size_t) pad];
        const auto* buf = padSample[(size_t) pad].get();
        if (buf == nullptr) return s;
        const double fileRate = buf->sourceSampleRate / sampleRate;

        if (st.slice > 0)
        {
            // SLICE n: that slice of the pad's break (equal 1/16ths if it has no
            // markers), one-shot from its start, at the pad's own pitch.
            const int len = buf->getNumSamples();
            const auto table = padSlices[(size_t) pad].isEmpty() ? SliceTable::equal (len, 16) : padSlices[(size_t) pad];
            const auto r = table.region ((st.slice - 1) % juce::jmax (1, table.count), len);
            s.regionStart = r.getStart();
            s.regionEnd   = juce::jmax (r.getStart() + kMinRegion, r.getEnd());
            s.loop = false;
            s.syncPeriodBeats = 0.0;
            s.increment = fileRate * std::pow (2.0, (double) padPitchNow[(size_t) pad] / 12.0);
        }
        if (st.pitch != 0)
        {
            // A pitched hit runs free at its own speed (off the loop grid for this hit).
            s.syncPeriodBeats = 0.0;
            s.increment *= std::pow (2.0, (double) st.pitch / 12.0);
        }
        if (st.rev) s.reverse = ! s.reverse;
        return s;
    }

    void AudioEngine::startVoice (int pad, float level, int sampleOffset, double startAt,
                                  const PadVoice::Settings* custom) noexcept
    {
        const auto& sample = padSample[(size_t) pad];
        if (sample == nullptr) return;

        const bool   playing  = transportNow != nullptr && transportNow->isPlaying;
        const double ppqHere  = transportNow != nullptr ? transportNow->ppqAt (sampleOffset) : 0.0;

        // Retrigger: the sounding voice fades out over 5 ms while the other
        // slot starts fresh. If that slot is still fading from an earlier hit
        // it is cut — at most a few ms of an already-fading tail.
        auto& slots = voices[(size_t) pad];
        auto& cur   = currentVoice[(size_t) pad];

        slots[(size_t) cur].release();
        cur = (cur + 1) % kVoicesPerPad;
        slots[(size_t) cur].kill();
        slots[(size_t) cur].start (sample, custom != nullptr ? *custom : padSettings[(size_t) pad], level, ppqHere, playing, startAt);
        voiceCustom[(size_t) pad][(size_t) cur] = custom != nullptr;
    }

    //==========================================================================
    //  SWAP groups
    //==========================================================================
    int AudioEngine::swapOwner (int group) const noexcept
    {
        const int p = swapPlaying[(size_t) group];
        if (p < 0 || padSwap[(size_t) p] != group) return -1;
        if (padSliceSeq[(size_t) p]) return sliceRun[(size_t) p].running ? p : -1;
        const auto& v = voices[(size_t) p][(size_t) currentVoice[(size_t) p]];
        return v.isActive() && ! v.isReleasing() ? p : -1;
    }

    int AudioEngine::samplesUntilSwap (int owner, int swapAt, int sampleOffset) const noexcept
    {
        const auto* t = transportNow;

        // 1/16, BEAT, BAR: the next grid line of Ableton's timeline.
        if (swapAt != params::swapAtSlice && t != nullptr && t->isPlaying && t->ppqPerSample > 0.0)
        {
            const double q   = swapAt == params::swapAtSixteenth ? 0.25 : (swapAt == params::swapAtBeat ? 1.0 : t->ppqPerBar());
            const double now = t->ppqAt (sampleOffset);
            const double target = std::ceil (now / q - 1.0e-9) * q;
            return (int) std::lround ((target - now) / t->ppqPerSample);
        }

        // A running slice sequence: its next step.
        if (padSliceSeq[(size_t) owner] && sliceRun[(size_t) owner].running && clockDppq > 0.0)
        {
            std::array<SliceStep, SliceSeqData::kSteps> pat;
            SliceSlot slot;
            slicePattern (owner, sliceRun[(size_t) owner].slot, pat, slot);
            const double stepPpq = kSliceRateBeats[slot.rate];
            const double now = clockAt (sampleOffset);
            const double next = std::ceil (now / stepPpq - 1.0e-9) * stepPpq;
            return (int) std::lround ((next - now) / clockDppq);
        }

        // SLICE (and any setting while Live is stopped): the next slice
        // point of the break that's playing, in its playback direction.
        const auto& v   = voices[(size_t) owner][(size_t) currentVoice[(size_t) owner]];
        const auto* buf = v.getBuffer();
        const int lo = v.getRegionStart(), hi = v.getRegionEnd();
        if (buf == nullptr || hi - lo < 2) return 0;

        std::array<int, 66> points {};
        int n = 0;
        points[(size_t) n++] = lo;
        const auto& table = padSlices[(size_t) owner];
        if (! table.isEmpty())
        {
            for (int i = 0; i < table.count; ++i)
                if (table.starts[(size_t) i] > lo && table.starts[(size_t) i] < hi)
                    points[(size_t) n++] = table.starts[(size_t) i];
        }
        else
        {
            // Unsliced: every 1/16 of the region.
            const double beats = PadVoice::loopBeatsFor ((double) (hi - lo) / buf->sourceSampleRate);
            const int steps = juce::jlimit (1, 64, (int) std::lround (beats * 4.0));
            for (int k = 1; k < steps; ++k)
                points[(size_t) n++] = lo + (int) ((juce::int64) (hi - lo) * k / steps);
        }

        const double pos = v.getPosition();
        double dist = -1.0;
        if (! v.isReverse())
        {
            for (int i = 0; i < n && dist < 0.0; ++i)
                if ((double) points[(size_t) i] > pos + 0.5) dist = (double) points[(size_t) i] - pos;
            if (dist < 0.0) dist = (double) hi - pos;              // the loop point / the end
        }
        else
        {
            for (int i = n; --i >= 0 && dist < 0.0;)
                if ((double) points[(size_t) i] < pos - 0.5) dist = pos - (double) points[(size_t) i];
            if (dist < 0.0) dist = pos - (double) lo + 1.0;
        }
        return (int) std::lround (juce::jmax (0.0, dist) / juce::jmax (1.0e-6, v.getIncrement()));
    }

    double AudioEngine::handoffPosition (int fromPad, int toPad, int sampleOffset) const noexcept
    {
        const auto& s   = padSettings[(size_t) toPad];
        const auto* buf = padSample[(size_t) toPad].get();
        const int len   = s.regionEnd - s.regionStart;
        if (buf == nullptr || len <= 0) return -1.0;

        // 1. The same musical point.
        double base;
        const bool playing = transportNow != nullptr && transportNow->isPlaying;
        if (s.loop && s.syncPeriodBeats > 0.0 && playing)
        {
            // Synced: where this break sits on Ableton's grid right now.
            base = PadVoice::gridPosition (s, transportNow->ppqAt (sampleOffset));
        }
        else
        {
            // Otherwise: the same fraction of the way through.
            double f = 0.0;
            if (fromPad >= 0)
            {
                const auto& v = voices[(size_t) fromPad][(size_t) currentVoice[(size_t) fromPad]];
                const double flen = (double) juce::jmax (1, v.getRegionEnd() - v.getRegionStart());
                f = v.isReverse() ? ((double) v.getRegionEnd() - 1.0 - v.getPosition()) / flen
                                  : (v.getPosition() - (double) v.getRegionStart()) / flen;
                f -= std::floor (f);
            }
            base = s.reverse ? (double) s.regionEnd - 1.0 - f * len : (double) s.regionStart + f * len;
        }

        // 2. Snap onto this break's own nearest slice marker (within a 1/32
        //    note) so it starts on its hit instead of a few ms into it.
        const auto& table = padSlices[(size_t) toPad];
        if (! s.reverse && ! table.isEmpty())
        {
            const double beats = PadVoice::loopBeatsFor ((double) len / buf->sourceSampleRate);
            const double tolerance = (double) len / (beats * 8.0);
            double best = -1.0, bestDistance = tolerance;
            auto consider = [&] (int m)
            {
                if (m < s.regionStart || m >= s.regionEnd) return;
                double d = std::abs ((double) m - base);
                if (s.loop) d = juce::jmin (d, (double) len - d);   // across the loop point
                if (d <= bestDistance) { bestDistance = d; best = (double) m; }
            };
            consider (s.regionStart);
            for (int i = 0; i < table.count; ++i) consider (table.starts[(size_t) i]);
            if (best >= 0.0) base = best;
        }
        return base;
    }

    void AudioEngine::fireSwap (int group, int sampleOffset) noexcept
    {
        auto& pending = pendingSwap[(size_t) group];
        if (! pending.active) return;
        pending.active = false;

        const int owner = swapOwner (group);
        if (pending.pad < 0)
        {
            if (owner >= 0 && padSliceSeq[(size_t) owner]) stopSliceSeq (owner);
            else if (owner >= 0)
                for (auto& v : voices[(size_t) owner]) v.release (3.0f);
            swapPlaying[(size_t) group] = -1;
            return;
        }

        const int to = pending.pad;
        if (padSample[(size_t) to] == nullptr) return;

        const double at = padSliceSeq[(size_t) to] ? -1.0 : handoffPosition (owner, to, sampleOffset);

        // The outgoing break was cut on its own slice point (1 ms before its
        // next hit), so a 1 ms fade is clean and nothing of that hit leaks.
        if (owner >= 0 && owner != to)
        {
            if (padSliceSeq[(size_t) owner]) stopSliceSeq (owner);
            for (auto& v : voices[(size_t) owner]) v.release (1.0f);
        }

        swapPlaying[(size_t) group] = to;
        padFlash[(size_t) to] = true;
        startPad (to, pending.level, sampleOffset, at);
    }

    void AudioEngine::releasePad (int pad) noexcept
    {
        // One-shots (and slices) ignore note-off; loops play while held.
        // Pads in a SWAP group latch: they play until another pad takes over.
        if (! juce::isPositiveAndBelow (pad, kNumPads) || padMode[(size_t) pad] != params::modeLoop
             || padSwap[(size_t) pad] > 0 || padSliceSeq[(size_t) pad]) return;
        for (auto& v : voices[(size_t) pad])
            if (v.isLooping()) v.release();
    }

    void AudioEngine::panic() noexcept
    {
        for (auto& padVoices : voices)
            for (auto& v : padVoices)
                v.release();       // 5 ms fade, never a hard cut

        pendingSwap.fill ({});
        swapPlaying.fill (-1);
        for (int p = 0; p < kNumPads; ++p)
            if (sliceRun[(size_t) p].running) stopSliceSeq (p);

        // Effects: stop repeats / dives / tails, forget MIDI latches.
        for (int g = 0; g < kNumGroups; ++g)
        {
            groups[(size_t) g].panicFx();
            fxLatch[(size_t) g].store (0);
            fxHeld[(size_t) g] = 0;
            fxStepHeld[(size_t) g] = 0;
        }

        // Later phases add: sequencer voices, stutter / beat-repeat buffers,
        // delay & reverb tails are cleared here too.
    }

    void AudioEngine::handleMidi (const juce::MidiMessage& m) noexcept
    {
        if (m.isNoteOn())
        {
            bridge.lastMidiNote.store (m.getNoteNumber(), std::memory_order_relaxed);
            bridge.lastMidiVelocity.store (m.getVelocity(), std::memory_order_relaxed);
            bridge.midiEventCount.fetch_add (1, std::memory_order_relaxed);

            // MIDI learn: this note now belongs to the pad being learned.
            const int learning = learnPad.exchange (-1);
            if (learning >= 0) setPadNote (learning, m.getNoteNumber());

            const int pad = padForNote (m.getNoteNumber());
            if (pad >= 0) { recordHit (pad, m.getFloatVelocity(), currentOffset); triggerPad (pad, m.getFloatVelocity(), currentOffset); }
            else          handleFxNote (m.getNoteNumber(), true);
        }
        else if (m.isNoteOff())
        {
            const int pad = padForNote (m.getNoteNumber());
            if (pad >= 0) releasePad (pad);
            else          handleFxNote (m.getNoteNumber(), false);
        }
        else if (m.isProgramChange())  // Program Change 1-8: recall a snapshot (message thread does it)
        {
            bridge.programChange.store (m.getProgramChangeNumber() + 1, std::memory_order_relaxed);
        }
        else if (m.isAllSoundOff())   // CC 120: a hardware panic button
        {
            panic();
        }
    }

    //==========================================================================
    //  Sequencer
    //==========================================================================
    void AudioEngine::collectSequencerEvents (const TransportState& t, const BlockParams& bp, int numSamples) noexcept
    {
        seqEventCount = 0;
        seqCursor = 0;
        blockLength = numSamples;

        // Slice-sequencer clock: Live's when it plays, free-running when stopped.
        clockPlaying = t.isPlaying && t.ppqPerSample > 0.0;
        clock0    = clockPlaying ? t.ppqAtBlockStart : freePpq;
        clockDppq = clockPlaying ? t.ppqPerSample : t.bpm / 60.0 / sampleRate;
        for (int p = 0; p < kNumPads; ++p)
            if (sliceRun[(size_t) p].running) addSliceEvents (p, 0);

        // Break bank: a new active break lands now, or on the next beat / bar.
        if (bankWanted != bankActive)
        {
            if (bankAtNow == params::switchNow || ! clockPlaying)
            {
                switchBank (bankWanted);
            }
            else
            {
                const double p0 = t.ppqAtBlockStart;
                const double unit   = bankAtNow == params::switchBeat ? 1.0 : t.ppqPerBar();
                const double origin = bankAtNow == params::switchBeat ? 0.0 : t.ppqOfBarStart;
                const double next   = origin + std::ceil ((p0 - origin) / unit - 1.0e-9) * unit;
                const int offset = (int) std::lround ((next - p0) / t.ppqPerSample);
                if (offset < numSamples)
                    insertEvent ({ juce::jmax (0, offset), bankWanted, 0.0f, SeqKind::bankSwitch, 0, 0 });
            }
        }
        seqLengthNow  = juce::jlimit (1, SequencerData::kSteps, bp.seqLength);
        seqStepPpq    = params::seqResPpq[juce::jlimit (0, 2, bp.seqRes)];
        seqQuantPpq   = params::seqQuantPpq[juce::jlimit (0, 3, bp.seqQuant)];
        seqRecording  = t.isPlaying && t.ppqPerSample > 0.0;

        if (! seqRecording)
        {
            bridge.seqStep.store (-1, std::memory_order_relaxed);
            if (seqWasPlaying) clearStepFx();          // Live stopped: step-held FX let go
            seqWasPlaying = false;
            return;
        }

        // Steps are on Live's absolute grid (step k starts at k * stepPpq), so a
        // restart, a loop-brace wrap or a relocate lands on the right step.
        const double p0 = t.ppqAtBlockStart;
        const double p1 = p0 + numSamples * t.ppqPerSample;
        const int64_t L = seqLengthNow;
        const auto mod  = [L] (int64_t k) { return (int) (((k % L) + L) % L); };
        const auto pass = [L] (int64_t k) { return k >= 0 ? k / L : -((-k + L - 1) / L); };
        const int64_t kNow = (int64_t) std::floor (p0 / seqStepPpq + 1.0e-9);
        bridge.seqStep.store (mod (kNow), std::memory_order_relaxed);
        if (! seqWasPlaying) seqFirstPass = pass (kNow);       // Live just started: this is the FIRST pass
        seqWasPlaying = true;

        if (! bp.seqRun) { clearStepFx(); return; }

        auto& data = bridge.seq;
        const int64_t kFirst = (int64_t) std::floor (p0 / seqStepPpq) - 1;     // micro-timing can pull a step earlier
        const int64_t kLast  = (int64_t) std::ceil  (p1 / seqStepPpq) + 1;
        auto inBlock  = [&] (double ppq) { return ppq >= p0 - 1.0e-9 && ppq < p1 - 1.0e-9; };
        auto offsetOf = [&] (double ppq) { return juce::jlimit (0, numSamples - 1, (int) std::lround ((ppq - p0) / t.ppqPerSample)); };

        for (int64_t k = kFirst; k <= kLast; ++k)
        {
            const int s = mod (k);
            for (int pad = 0; pad < kNumPads; ++pad)
            {
                const auto st = data.get (pad, s);
                if (! st.on || k == seqSkipOnce[(size_t) pad]) continue;

                const double when = ((double) k + st.micro / 128.0) * seqStepPpq;
                const double end  = when + seqStepPpq;
                const int r = juce::jlimit (1, 4, st.ratchet);
                const double sub = seqStepPpq / r;

                // Anything of this step in the block? (first hit, a ratchet, the FX release)
                bool touches = st.fx > 0 && inBlock (end);
                for (int j = 0; j < r && ! touches; ++j) touches = inBlock (when + j * sub);
                if (! touches) continue;

                // CONDITION (pattern pass on Live's timeline) and CHANCE, decided
                // once when the step's first hit comes due; ratchets follow it.
                if (inBlock (when))
                {
                    bool plays = seqConditionPasses (st.cond, pass (k), bp.seqFill, pass (k) == seqFirstPass);
                    if (plays && st.probability < 100) plays = seqRandom.nextInt (100) < st.probability;
                    seqRollStep[(size_t) pad]  = k;
                    seqRollPlays[(size_t) pad] = plays;
                }
                if (seqRollStep[(size_t) pad] != k || ! seqRollPlays[(size_t) pad]) continue;

                const uint64_t packed = st.pack();
                for (int j = 0; j < r && st.hit; ++j)
                    if (inBlock (when + j * sub))
                        insertEvent ({ offsetOf (when + j * sub), pad, (float) st.velocity / 127.0f, SeqKind::padHit, packed, 0 });

                // FX: held on the pad's group for the length of the step.
                if (st.fx > 0)
                {
                    const int g = padGroup[(size_t) pad];
                    uint64_t endBits;
                    std::memcpy (&endBits, &end, sizeof end);
                    if (inBlock (when)) insertEvent ({ offsetOf (when), g, 0.0f, SeqKind::stepFxOn,  endBits, (uint32_t) (st.fx - 1) });
                    if (inBlock (end))  insertEvent ({ offsetOf (end),  g, 0.0f, SeqKind::stepFxOff, endBits, (uint32_t) (st.fx - 1) });
                }
            }
        }
    }

    void AudioEngine::applyFx (int g) noexcept
    {
        groups[(size_t) g].setFx ((fxParamMask[(size_t) g] ^ fxLatch[(size_t) g].load (std::memory_order_relaxed))
                                    | fxHeld[(size_t) g] | fxStepHeld[(size_t) g],
                                  fxParams[(size_t) g]);
    }

    void AudioEngine::stepFx (int g, int f, bool on, double endPpq) noexcept
    {
        if (! juce::isPositiveAndBelow (g, kNumGroups) || ! juce::isPositiveAndBelow (f, fx::kNumFx)) return;
        auto& until = fxStepEnd[(size_t) g][(size_t) f];
        const uint32_t bit = 1u << f;
        if (on)
        {
            until = (fxStepHeld[(size_t) g] & bit) != 0 ? juce::jmax (until, endPpq) : endPpq;
            fxStepHeld[(size_t) g] |= bit;
        }
        else
        {
            // Only the step that holds it last lets go (a later step may have taken it on).
            if ((fxStepHeld[(size_t) g] & bit) == 0 || until > endPpq + 1.0e-9) return;
            fxStepHeld[(size_t) g] &= ~bit;
        }
        applyFx (g);
    }

    void AudioEngine::clearStepFx() noexcept
    {
        for (int g = 0; g < kNumGroups; ++g)
            if (fxStepHeld[(size_t) g] != 0) { fxStepHeld[(size_t) g] = 0; applyFx (g); }
    }

    void AudioEngine::insertEvent (const SeqEvent& e) noexcept
    {
        // Sorted by offset, then kind (releases before hits on a tie). Stable.
        if (seqEventCount >= kMaxSeqEvents) return;
        int i = seqEventCount++;
        while (i > seqCursor && (seqEvents[(size_t) i - 1].offset > e.offset
                         || (seqEvents[(size_t) i - 1].offset == e.offset && seqEvents[(size_t) i - 1].kind > e.kind)))
        {
            seqEvents[(size_t) i] = seqEvents[(size_t) i - 1];
            --i;
        }
        seqEvents[(size_t) i] = e;
    }

    //==========================================================================
    //  Slice sequencer
    //==========================================================================
    int AudioEngine::slicePattern (int pad, int slot, std::array<SliceStep, SliceSeqData::kSteps>& pat, SliceSlot& settings) const noexcept
    {
        auto& data = bridge.sliceSeq;
        settings = data.getSlot (pad, slot);
        if (settings.written)
        {
            for (int k = 0; k < SliceSeqData::kSteps; ++k) pat[(size_t) k] = data.getStep (pad, slot, k);
            return settings.length;
        }
        // Never written: the break as recorded.
        const auto* buf = padSample[(size_t) pad].get();
        if (buf == nullptr) { pat.fill ({}); return settings.length; }
        const double beats = PadVoice::loopBeatsFor ((double) buf->getNumSamples() / buf->sourceSampleRate);
        settings.length = SliceSeqData::defaultPattern (padSlices[(size_t) pad], buf->getNumSamples(), buf->sourceSampleRate,
                                                        kSliceRateBeats[settings.rate], beats, pat);
        return settings.length;
    }

    void AudioEngine::startSliceSeq (int pad, int sampleOffset) noexcept
    {
        auto& run = sliceRun[(size_t) pad];
        if (padSample[(size_t) pad] == nullptr) return;
        run.running = true;
        run.slot = padSliceSlot[(size_t) pad];
        ++run.gen;

        std::array<SliceStep, SliceSeqData::kSteps> pat;
        SliceSlot slot;
        slicePattern (pad, run.slot, pat, slot);
        const double stepPpq = kSliceRateBeats[slot.rate];

        if (clockPlaying)
        {
            run.anchor = 0;                      // on Live's grid: enters where the bar says
        }
        else
        {
            bool others = false;
            for (int p = 0; p < kNumPads; ++p) others = others || (p != pad && sliceRun[(size_t) p].running);
            if (! others)
            {
                // Live stopped, nothing else running: start the free clock here,
                // so step 1 plays right now.
                clock0 = -sampleOffset * clockDppq;
                run.anchor = 0;
            }
            else
            {
                run.anchor = (int64_t) std::ceil (clockAt (sampleOffset) / stepPpq - 1.0e-9);   // join the others' grid
            }
        }
        addSliceEvents (pad, sampleOffset);
    }

    void AudioEngine::stopSliceSeq (int pad) noexcept
    {
        auto& run = sliceRun[(size_t) pad];
        run.running = false;
        ++run.gen;
        for (auto& v : voices[(size_t) pad]) v.release (3.0f);
        bridge.sliceStep[(size_t) pad].store (-1, std::memory_order_relaxed);
    }

    void AudioEngine::addSliceEvents (int pad, int fromOffset) noexcept
    {
        auto& run = sliceRun[(size_t) pad];
        if (! run.running || clockDppq <= 0.0) return;

        std::array<SliceStep, SliceSeqData::kSteps> pat;
        SliceSlot slot;
        int L = slicePattern (pad, run.slot, pat, slot);
        double stepPpq = kSliceRateBeats[slot.rate];
        run.stepPpq = stepPpq;

        const double p0 = clockAt (fromOffset), p1 = clockAt (blockLength);
        const bool fill = bridge.sliceFill[(size_t) pad].load (std::memory_order_relaxed);
        const int64_t kFirst = (int64_t) std::floor (p0 / stepPpq) - 1;
        int64_t kLast = (int64_t) std::ceil (p1 / stepPpq) + 1;
        auto offsetOf = [this] (double ppq) { return juce::jlimit (0, blockLength - 1, (int) std::lround ((ppq - clock0) / clockDppq)); };
        auto inBlock  = [&] (double ppq) { return ppq >= p0 - 1.0e-9 && ppq < p1 - 1.0e-9; };

        for (int64_t k = kFirst; k <= kLast; ++k)
        {
            if (k < run.anchor) continue;
            int idx = (int) (((k - run.anchor) % L + L) % L);

            // Slot change (automation / SLOT readout) lands at the pattern start.
            if (idx == 0 && padSliceSlot[(size_t) pad] != run.slot && inBlock (k * stepPpq))
            {
                run.slot = padSliceSlot[(size_t) pad];
                L = slicePattern (pad, run.slot, pat, slot);
                const double newStep = kSliceRateBeats[slot.rate];
                if (std::abs (newStep - stepPpq) > 1.0e-12)
                {
                    // Different step size: re-anchor so this moment is step 0 of the new slot.
                    run.anchor = (int64_t) std::llround (k * stepPpq / newStep);
                    stepPpq = newStep;
                    run.stepPpq = stepPpq;
                    kLast = (int64_t) std::ceil (p1 / stepPpq) + 1;
                    k = run.anchor - 1;
                    continue;
                }
            }

            const auto& st = pat[(size_t) idx];
            const bool odd = ((k - run.anchor) & 1) != 0;
            const double base = k * stepPpq + (odd ? slot.swing / 100.0 * 0.5 * stepPpq : 0.0);
            const double when = base + st.micro / 64.0 * stepPpq;

            if (st.kind == SliceStep::rest)
            {
                if (inBlock (when)) insertEvent ({ offsetOf (when), pad, 0.0f, SeqKind::sliceRelease, 0, run.gen });
                continue;
            }

            SliceStep hit = st;
            if (st.kind == SliceStep::tie)
            {
                if (! fill) continue;
                hit = SliceStep::playing (run.lastSlice);     // FILL: roll the slice that's ringing
                hit.ratchet = 2;
            }
            else if (fill)
            {
                hit.ratchet = 4;
            }

            const int r = juce::jlimit (1, 4, hit.ratchet);
            const double sub = stepPpq / r;
            const float frac = kSliceLengthFrac[juce::jlimit (0, 3, hit.length)];
            for (int j = 0; j < r; ++j)
            {
                const double t = when + j * sub;
                if (inBlock (t))
                {
                    // Chance is rolled once per step, when its first hit comes due.
                    if (j == 0 && hit.chance < 100 && seqRandom.nextInt (100) >= hit.chance) break;
                    insertEvent ({ offsetOf (t), pad, (float) hit.velocity / 127.0f, SeqKind::sliceHit, hit.pack(), run.gen });
                }
                if (frac < 1.0f && inBlock (t + sub * frac))
                    insertEvent ({ offsetOf (t + sub * frac), pad, 0.0f, SeqKind::sliceRelease, 0, run.gen });
            }
        }
    }

    void AudioEngine::sliceHit (int pad, const SliceStep& st, float velocity, int sampleOffset) noexcept
    {
        const auto* buf = padSample[(size_t) pad].get();
        if (buf == nullptr) return;
        const int len = buf->getNumSamples();
        const auto table = padSlices[(size_t) pad].isEmpty() ? SliceTable::equal (len, 16) : padSlices[(size_t) pad];
        const auto r = table.region (st.slice, len);

        // Play from the slice start on through the break (the next hit or a
        // REST cuts it; TIE lets it ring), or backwards from the slice end.
        PadVoice::Settings s = padSettings[(size_t) pad];
        s.loop = false;
        s.syncPeriodBeats = 0.0;
        s.reverse = st.rev;
        s.regionStart = st.rev ? 0 : r.getStart();
        s.regionEnd   = st.rev ? juce::jmax (32, r.getEnd()) : len;
        s.increment   = buf->sourceSampleRate / sampleRate * std::pow (2.0, (double) (padPitchNow[(size_t) pad] + (float) st.pitch) / 12.0);

        sliceRun[(size_t) pad].lastSlice = st.slice;
        padFlash[(size_t) pad] = true;

        // Continuation: the break is already playing right here (the slice
        // that follows the one before, as recorded). Retriggering would only
        // double it for a few ms, so let it run on. Tolerance: half a
        // micro-timing step, so the default pattern replays the break exactly.
        const auto& cur = voices[(size_t) pad][(size_t) currentVoice[(size_t) pad]];
        if (! st.rev && st.pitch == 0 && cur.isActive() && ! cur.isReleasing() && ! cur.isReverse()
            && cur.getBuffer() == buf && cur.getRegionEnd() == len && std::abs (cur.getIncrement() - s.increment) < 1.0e-9
            && clockDppq > 0.0)
        {
            const double stepSrc = sliceRun[(size_t) pad].stepPpq / clockDppq * s.increment;
            if (std::abs (cur.getPosition() - (double) r.getStart()) <= stepSrc / 128.0 + 2.0)
                return;
        }

        startVoice (pad, levelFor (pad, velocity), sampleOffset, -1.0, &s);
    }

    void AudioEngine::recordHit (int pad, float velocity, int sampleOffset) noexcept
    {
        if (! seqRecording || transportNow == nullptr || ! bridge.seq.isArmed (pad)) return;

        // Quantise to the record grid, then express it on the step grid; anything
        // finer than a step (1/32 on a 1/16 grid) is kept as micro-timing.
        const double ppq = transportNow->ppqAt (sampleOffset);
        const double q   = std::round (ppq / seqQuantPpq) * seqQuantPpq;
        const double x   = q / seqStepPpq;
        const int64_t k  = (int64_t) std::llround (x);
        const int micro  = juce::jlimit (-64, 63, (int) std::lround ((x - (double) k) * 128.0));

        // Recording over a step keeps its settings (pitch, ratchet, FX ...).
        const int64_t L = seqLengthNow;
        const int idx = (int) (((k % L) + L) % L);
        SeqStep st = bridge.seq.get (pad, idx);
        if (! st.on) st = {};
        st.on = true;
        st.hit = true;
        st.velocity = juce::jlimit (1, 127, juce::roundToInt (velocity * 127.0f));
        st.micro = micro;
        bridge.seq.set (pad, idx, st);

        // That step may still be ahead in this pass (hit a little early):
        // you've just played it, so the sequencer mustn't play it again now.
        seqSkipOnce[(size_t) pad] = k;
    }

    bool AudioEngine::handleFxNote (int note, bool on) noexcept
    {
        const int f = note - fx::kFirstMidiNote;
        if (! juce::isPositiveAndBelow (f, fx::kNumFx)) return false;
        const int g = fxTargetGroup.load (std::memory_order_relaxed);
        const uint32_t bit = 1u << f;

        if ((fxLocked.load (std::memory_order_relaxed) & bit) != 0)
        {
            if (on) fxLatch[(size_t) g].fetch_xor (bit, std::memory_order_relaxed);   // LOCKED: each press flips it
        }
        else
        {
            if (on) fxHeld[(size_t) g] |= bit;                                        // MOMENTARY: on while held
            else    fxHeld[(size_t) g] &= ~bit;
        }

        // Apply now (sample-accurate) rather than next block.
        applyFx (g);
        return true;
    }

    void AudioEngine::clearFxLatch (int group, int fxId) noexcept
    {
        if (juce::isPositiveAndBelow (group, kNumGroups) && juce::isPositiveAndBelow (fxId, fx::kNumFx))
            fxLatch[(size_t) group].fetch_and (~(1u << fxId));
    }

    void AudioEngine::renderVoices (juce::AudioBuffer<float>& buffer, int start, int num) noexcept
    {
        // Queued SWAP hand-offs land sample-accurately: split the span there.
        while (num > 0)
        {
            int span = num;
            for (int g = 1; g <= kSwapGroups; ++g)
                if (pendingSwap[(size_t) g].active)
                    span = juce::jmin (span, juce::jmax (0, pendingSwap[(size_t) g].remaining));

            if (span > 0)
            {
                renderSpan (buffer, start, span);

                for (int g = 1; g <= kSwapGroups; ++g)
                    if (pendingSwap[(size_t) g].active)
                        pendingSwap[(size_t) g].remaining -= span;
                start += span;
                num   -= span;
            }

            for (int g = 1; g <= kSwapGroups; ++g)
                if (pendingSwap[(size_t) g].active && pendingSwap[(size_t) g].remaining <= 0)
                    fireSwap (g, start);
        }
    }

    void AudioEngine::renderSpan (juce::AudioBuffer<float>& buffer, int start, int num) noexcept
    {
        // Voices -> their group's bus -> (FX) -> level / pan -> master buffer.
        // Spans longer than a bus can hold are done in pieces.
        while (num > 0)
        {
            const int n = juce::jmin (num, groups[0].getCapacity());

            std::array<juce::AudioBuffer<float>*, kNumGroups> inputs {};
            for (int g = 0; g < kNumGroups; ++g)
                inputs[(size_t) g] = &groups[(size_t) g].beginSpan (n);

            for (int p = 0; p < kNumPads; ++p)
            {
                const int g = padGroup[(size_t) p];
                for (auto& v : voices[(size_t) p])
                    if (v.isActive())
                    {
                        groups[(size_t) g].markUsed();
                        padPeak[(size_t) p] = juce::jmax (padPeak[(size_t) p], v.renderAdding (*inputs[(size_t) g], 0, n));
                    }
            }

            FxClock clock;
            if (transportNow != nullptr)
            {
                clock.ppq          = transportNow->ppqAt (start);
                clock.ppqPerSample = transportNow->ppqPerSample;
                clock.bpm          = transportNow->bpm;
                clock.ppqPerBar    = transportNow->ppqPerBar();
                clock.playing      = transportNow->isPlaying && transportNow->ppqPerSample > 0.0;
            }
            for (int g = 0; g < kNumGroups; ++g)
                groupPeak[(size_t) g] = juce::jmax (groupPeak[(size_t) g], groups[(size_t) g].mixInto (buffer, start, n, clock));

            start += n;
            num   -= n;
        }
    }

    //==========================================================================
    void AudioEngine::process (juce::AudioBuffer<float>& buffer,
                               const juce::MidiBuffer& midi,
                               const TransportState& transport,
                               const BlockParams& params) noexcept
    {
        const int numSamples = buffer.getNumSamples();
        buffer.clear();
        padPeak.fill (0.0f);
        padFlash.fill (false);
        groupPeak.fill (0.0f);

        // ---- New / cleared samples from the loader, then this block's pad settings.
        transportNow  = &transport;
        currentOffset = 0;
        applyPadCommands();
        computeVoiceSettings (params);

        // ---- Synced loops re-lock to the host's position at the block start.
        if (transport.isPlaying)
            for (auto& padVoices : voices)
                for (auto& v : padVoices)
                    v.syncTo (transport.ppqAtBlockStart);

        // ---- Panic: UI button or automatable parameter rising edge.
        //      (Transport stop is not a panic: tails ring out naturally.)
        const bool panicEdge = params.panicParam && ! lastPanicParam;
        lastPanicParam = params.panicParam;
        if (bridge.panicRequested.exchange (false) || panicEdge)
            panic();

        // ---- Sequencer steps due in this block (sample positions, sorted).
        collectSequencerEvents (transport, params, numSamples);

        // ---- Mouse pad presses from the UI land at the start of the block.
        PadUiEvent ev;
        while (bridge.padEvents.pop (ev))
        {
            if (ev.noteOn) { recordHit (ev.pad, ev.velocity, 0); triggerPad (ev.pad, ev.velocity, 0); }
            else           releasePad (ev.pad);
        }

        // ---- Sample-accurate MIDI and sequencer steps, in time order: render
        //      up to each event, then apply it (steps first on a tie).
        int pos = 0;
        auto playStepsUpTo = [&] (int limit)
        {
            while (seqCursor < seqEventCount && seqEvents[(size_t) seqCursor].offset <= limit)
            {
                auto e = seqEvents[(size_t) seqCursor++];
                e.offset = juce::jmax (pos, e.offset);      // never render backwards
                renderVoices (buffer, pos, e.offset - pos);
                pos = e.offset;
                currentOffset = e.offset;
                switch (e.kind)
                {
                    case SeqKind::bankSwitch:
                        switchBank (e.pad);
                        break;
                    case SeqKind::padHit:
                    {
                        const auto st = SeqStep::unpack (e.step);
                        triggerPad (e.pad, e.velocity, e.offset, true, &st);
                        break;
                    }
                    case SeqKind::stepFxOn:
                    case SeqKind::stepFxOff:
                    {
                        double end;
                        std::memcpy (&end, &e.step, sizeof end);
                        stepFx (e.pad, (int) e.gen, e.kind == SeqKind::stepFxOn, end);
                        break;
                    }
                    case SeqKind::sliceHit:
                        if (sliceRun[(size_t) e.pad].running && sliceRun[(size_t) e.pad].gen == e.gen)
                            sliceHit (e.pad, SliceStep::unpack (e.step), e.velocity, e.offset);
                        break;
                    case SeqKind::sliceRelease:
                        if (sliceRun[(size_t) e.pad].running && sliceRun[(size_t) e.pad].gen == e.gen)
                            for (auto& v : voices[(size_t) e.pad]) v.release (3.0f);
                        break;
                }
            }
        };
        for (const auto meta : midi)
        {
            const int eventPos = juce::jlimit (0, numSamples, meta.samplePosition);
            playStepsUpTo (eventPos);
            renderVoices (buffer, pos, eventPos - pos);
            pos = eventPos;
            currentOffset = eventPos;
            handleMidi (meta.getMessage());
        }
        playStepsUpTo (numSamples - 1);
        renderVoices (buffer, pos, numSamples - pos);

        // Slice-sequencer playheads (UI), and the free clock moves on.
        for (int p = 0; p < kNumPads; ++p)
        {
            const auto& run = sliceRun[(size_t) p];
            int step = -1;
            if (run.running && clockDppq > 0.0)
            {
                std::array<SliceStep, SliceSeqData::kSteps> pat;
                SliceSlot slot;
                const int L = slicePattern (p, run.slot, pat, slot);
                const int64_t k = (int64_t) std::floor (clockAt (numSamples - 1) / kSliceRateBeats[slot.rate] + 1.0e-9);
                if (k >= run.anchor) step = (int) (((k - run.anchor) % L + L) % L);
            }
            bridge.sliceStep[(size_t) p].store (step, std::memory_order_relaxed);
            bridge.sliceSlotNow[(size_t) p].store (run.slot, std::memory_order_relaxed);
        }
        freePpq = clock0 + numSamples * clockDppq;

        transportNow = nullptr;
        currentParams = nullptr;

        // ---- Master
        master.setGainDb (params.masterGainDb);
        master.setLimiterEnabled (params.limiterEnabled);
        const auto result = master.process (buffer, numSamples);

        // ---- Publish to the UI (relaxed atomics; display only)
        auto storeMax = [] (std::atomic<float>& a, float v)
        {
            float prev = a.load (std::memory_order_relaxed);
            while (v > prev && ! a.compare_exchange_weak (prev, v, std::memory_order_relaxed)) {}
        };
        storeMax (bridge.peakLeft,  result.peakLeft);
        for (int g = 0; g < kNumGroups; ++g)
        {
            storeMax (bridge.groupPeak[(size_t) g], groupPeak[(size_t) g]);
            bridge.fxEngaged[(size_t) g].store (groups[(size_t) g].getChain().engagedMask(), std::memory_order_relaxed);
            bridge.fxArmed[(size_t) g].store (groups[(size_t) g].getChain().armedMask(), std::memory_order_relaxed);
        }
        storeMax (bridge.peakRight, result.peakRight);
        bridge.limiterReductionDb.store (result.maxReductionDb, std::memory_order_relaxed);
        if (result.clipped) bridge.clipCount.fetch_add (1, std::memory_order_relaxed);

        // Lamp envelopes decay ~150 ms whatever the host block size is.
        const float decay = (float) std::exp (-(double) numSamples / (sampleRate * 0.15));
        for (int p = 0; p < kNumPads; ++p)
        {
            auto& a = bridge.padActivity[(size_t) p];
            float level = juce::jmin (1.0f, padPeak[(size_t) p] * 2.5f);
            if (padFlash[(size_t) p]) level = juce::jmax (level, 0.45f);
            a.store (juce::jmax (a.load (std::memory_order_relaxed) * decay, level), std::memory_order_relaxed);

            // Playhead of the most recently started voice, for the waveform cursor.
            const auto& v = voices[(size_t) p][(size_t) currentVoice[(size_t) p]];
            float head = -1.0f;
            if (v.isActive() && ! v.isReleasing() && v.getBuffer() != nullptr)
                head = (float) (v.getPosition() / (double) v.getBuffer()->getNumSamples());
            bridge.padPlayhead[(size_t) p].store (head, std::memory_order_relaxed);

            // Queued SWAP (incoming pad, or the playing pad waiting to stop).
            bool queued = false;
            for (int g = 1; g <= kSwapGroups; ++g)
            {
                const auto& ps = pendingSwap[(size_t) g];
                queued = queued || (ps.active && (ps.pad == p || (ps.pad < 0 && swapPlaying[(size_t) g] == p)));
            }
            bridge.padQueued[(size_t) p].store (queued, std::memory_order_relaxed);
            bridge.padLoaded[(size_t) p].store (padSample[(size_t) p] != nullptr, std::memory_order_relaxed);
        }

        // ---- Scope tap: decimated mono sum of the master output
        const auto* l = buffer.getReadPointer (0);
        const auto* r = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : l;
        int n = 0;
        for (int i = 0; i < numSamples && n < (int) scopeScratch.size(); ++i)
        {
            if (++scopeCounter >= scopeDecimation)
            {
                scopeCounter = 0;
                scopeScratch[(size_t) n++] = 0.5f * (l[i] + r[i]);
            }
        }
        bridge.pushScope (scopeScratch.data(), n);
    }
}
