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
            padNote[(size_t) p].store (kDefaultFirstNote + p % kPadsPerScene);    // both scenes on 36..51
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

        // RESAMPLE: the whole take and a group tap, allocated now (never on the audio thread).
        rsBuffer.setSize (2, (int) std::ceil (kMaxResampleSeconds * sampleRate));
        rsBuffer.clear();
        rsTap.setSize (2, juce::jmax (8192, maxBlockSize));
        rsState.store (rsIdle);

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
        laneRun.fill ({});
        for (auto& s : bridge.laneStep) s.store (-1);
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

        // One note -> one pad (within a scene): whoever had this note gets this pad's old one.
        if (note >= 0)
            for (int p = 0; p < kNumPads; ++p)
                if (p != pad && sceneOf (p) == sceneOf (pad) && padNote[(size_t) p].load() == note)
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
        // The scene on view first (a 16-pad controller plays what you see), then the other.
        const int first = sceneViewNow.load (std::memory_order_relaxed);
        for (int k = 0; k < kNumScenes; ++k)
        {
            const int scene = (first + k) % kNumScenes;
            for (int i = 0; i < kPadsPerScene; ++i)
            {
                const int p = scene * kPadsPerScene + i;
                if (padNote[(size_t) p].load (std::memory_order_relaxed) == note)
                    return p;
            }
        }
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

        // Scenes: crossfader (or a held cut key) -> a gain per scene, glided by the voices.
        sceneViewNow.store (juce::jlimit (0, kNumScenes - 1, bp.sceneView), std::memory_order_relaxed);
        {
            float a, b;
            const int cut = xfadeCut.load (std::memory_order_relaxed);
            params::xfadeGains (cut < 0 ? bp.xfade : (float) cut, cut < 0 ? bp.xfadeCurve : params::curveSharp, a, b);
            sceneGain = { a, b };
        }

        macroParamMask = bp.macroOn;
        macroAmountNow = juce::jlimit (0.0f, 1.0f, bp.macroAmount);
        macroGroupNow  = juce::jlimit (0, 4, bp.macroGroup);
        macroLatchNow  = bp.macroLatch;

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
            padStepsWhole[(size_t) p] = pp.stepsWhole;
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
            padLaunch[(size_t) p]  = juce::jlimit (0, 3, pp.launch);
            padSeqLen[(size_t) p]  = juce::jlimit (0, SequencerData::kSteps, pp.seqLen);
            padSwapFrom[(size_t) p] = juce::jlimit (0, 1, pp.swapFrom);
            if (laneRun[(size_t) p].running && ! params::isSeqMode (pp.mode))
                stopLane (p);                         // no longer a SEQ pad (edit, snapshot ...)

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
            s.character   = juce::jlimit (0, PadVoice::kNumCharacters - 1, pp.character);

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
            GroupBus::panGains (g * sceneGain[(size_t) sceneOf (p)], pp.pan, s.gainLeft, s.gainRight);

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
                if (laneRun[(size_t) p].running)  stopLane (p);
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
        // SEQ pads: startAt is the lane's anchor (clock ppq of its step 1), or -1 = its LAUNCH boundary.
        if (isSeqPad (pad))
        {
            // A SWAP hand-off passes the anchor (and starts right here); otherwise its LAUNCH boundary, on the grid.
            const double b = startAt >= 0.0 ? clockAt (sampleOffset) : launchBoundary (pad, sampleOffset);
            launchLane (pad, sampleOffset, startAt >= 0.0 ? startAt : gridAnchor (pad, b), b, level);
        }
        else if (padSliceSeq[(size_t) pad]) startSliceSeq (pad, sampleOffset);
        else                           startVoice (pad, level, sampleOffset, startAt);
    }

    void AudioEngine::triggerPad (int pad, float velocity, int sampleOffset, bool fromSequencer, const SeqStep* step) noexcept
    {
        if (! juce::isPositiveAndBelow (pad, kNumPads)) return;

        bridge.padTriggerCount[(size_t) pad].fetch_add (1, std::memory_order_relaxed);
        padFlash[(size_t) pad] = true;     // lamp blinks even on an empty pad

        chokeOthers (pad, padChoke[(size_t) pad]);

        const float level = levelFor (pad, velocity);

        // A step of a launched sequence plays the sound itself (no SWAP / launch logic).
        const bool laneHit = fromSequencer && isSeqPad (pad);

        // ---- SWAP group: queue a hand-off instead of starting now.
        const int group = padSwap[(size_t) pad];
        if (group > 0 && ! laneHit)
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

        // SEQ ONE / SEQ LOOP (A6): a hit launches the pad's own lane.
        if (isSeqPad (pad) && ! fromSequencer)
        {
            hitSeqPad (pad, velocity, sampleOffset);
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
        if (step != nullptr && (step->pitch != 0 || step->rev || (step->slice > 0 && ! padStepsWhole[(size_t) pad])))
        {
            const auto s = tweakedSettings (pad, *step);
            startVoice (pad, level, sampleOffset, -1.0, &s);
            return;
        }
        startVoice (pad, level, sampleOffset);
    }

    void AudioEngine::controllerSliceHit (int pad, int slice, float velocity, int sampleOffset) noexcept
    {
        // KEYBOARD on the Mikro (A8): slice n of the pad, straight to a voice (no SWAP / launch / slice seq).
        if (! juce::isPositiveAndBelow (pad, kNumPads) || padSample[(size_t) pad] == nullptr) return;
        bridge.padTriggerCount[(size_t) pad].fetch_add (1, std::memory_order_relaxed);
        padFlash[(size_t) pad] = true;
        chokeOthers (pad, padChoke[(size_t) pad]);
        if (slice <= 0) { startVoice (pad, levelFor (pad, velocity), sampleOffset); return; }
        SeqStep st;
        st.on = true;
        st.slice = juce::jlimit (1, 16, slice);
        const auto s = tweakedSettings (pad, st);
        startVoice (pad, levelFor (pad, velocity), sampleOffset, -1.0, &s);
    }

    void AudioEngine::addRollEvents (int numSamples) noexcept
    {
        // NOTE REPEAT (A8): while held, the pad hits again on every 1/8, 1/16 or 1/32 of
        // the clock (Live's grid, or the free clock when stopped), sample-accurately.
        static constexpr double kRollPpq[] = { 0.5, 0.25, 0.125 };
        if (clockDppq <= 0.0) return;
        const double c0 = clockAt (0), c1 = clockAt (numSamples);
        for (int p = 0; p < kNumPads; ++p)
        {
            const auto& r = rollRun[(size_t) p];
            if (! r.active) continue;
            const double step = kRollPpq[juce::jlimit (0, 2, r.rate)];
            for (double t = std::ceil ((c0 - 1.0e-9) / step) * step; t < c1 - 1.0e-9; t += step)
            {
                if (t < r.skipUntil) continue;          // the press itself just played: no flam
                const int off = juce::jlimit (0, numSamples - 1, (int) std::llround ((t - c0) / clockDppq));
                insertEvent ({ off, p, r.velocity, SeqKind::rollHit, (uint64_t) r.slice, 0, -1, 0 });
            }
        }
    }

    PadVoice::Settings AudioEngine::tweakedSettings (int pad, const SeqStep& st) const noexcept
    {
        PadVoice::Settings s = padSettings[(size_t) pad];
        const auto* buf = padSample[(size_t) pad].get();
        if (buf == nullptr) return s;
        const double fileRate = buf->sourceSampleRate / sampleRate;

        if (st.slice > 0 && ! padStepsWhole[(size_t) pad])       // (WHOLE: the pad plays whole whatever the step says)
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
        if (isSeqPad (p))            return laneRun[(size_t) p].running && ! laneRun[(size_t) p].releaseAtEnd ? p : -1;
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

        // A launched sequence (A6): its next STEP, or the grid on the free clock when Live is stopped.
        if (isSeqPad (owner) && laneRun[(size_t) owner].running && clockDppq > 0.0)
        {
            const double now = clockAt (sampleOffset);
            double q = seqStepPpq, origin = laneRun[(size_t) owner].anchor;
            if      (swapAt == params::swapAtSixteenth) { q = 0.25; origin = 0.0; }
            else if (swapAt == params::swapAtBeat)      { q = 1.0;  origin = 0.0; }
            else if (swapAt == params::swapAtBar)       { q = 4.0;  origin = 0.0; }
            const double next = origin + std::ceil ((now - origin) / q - 1.0e-9) * q;
            return (int) std::lround ((next - now) / clockDppq);
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
            if (owner >= 0 && isSeqPad (owner)) stopLane (owner, 1.0f);
            if (owner >= 0 && padSliceSeq[(size_t) owner]) stopSliceSeq (owner);
            else if (owner >= 0)
                for (auto& v : voices[(size_t) owner]) v.release (3.0f);
            swapPlaying[(size_t) group] = -1;
            return;
        }

        const int to = pending.pad;
        if (padSample[(size_t) to] == nullptr) return;

        double at;
        if (isSeqPad (to))
        {
            // A sequence taking over: SAME STEP carries on from the same position
            // (the outgoing sequence's, or the pattern's place on the grid); STEP 1
            // starts from the top, right here.
            // SAME STEP: the step under Live's playhead (the song grid); STEP 1: from the top, right here.
            const double now = clockAt (sampleOffset);
            at = padSwapFrom[(size_t) to] == 1 ? now : gridAnchor (to, now);
        }
        else
        {
            at = padSliceSeq[(size_t) to] ? -1.0 : handoffPosition (owner, to, sampleOffset);
        }

        // The outgoing break was cut on its own slice point (1 ms before its
        // next hit), so a 1 ms fade is clean and nothing of that hit leaks.
        if (owner >= 0 && owner != to)
        {
            if (isSeqPad (owner)) stopLane (owner, 1.0f);
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
             || padSwap[(size_t) pad] > 0 || padSliceSeq[(size_t) pad] || isSeqPad (pad)) return;
        for (auto& v : voices[(size_t) pad])
            if (v.isLooping()) v.release();
    }

    void AudioEngine::panic() noexcept
    {
        for (auto& padVoices : voices)
            for (auto& v : padVoices)
                v.release();       // 5 ms fade, never a hard cut

        for (auto& r : rollRun) r.active = false;
        pendingSwap.fill ({});
        swapPlaying.fill (-1);
        for (int p = 0; p < kNumPads; ++p)
        {
            if (sliceRun[(size_t) p].running) stopSliceSeq (p);
            if (laneRun[(size_t) p].running)  stopLane (p);
        }

        // Effects: stop repeats / dives / tails, forget MIDI latches.
        for (int g = 0; g < kNumGroups; ++g)
        {
            groups[(size_t) g].panicFx();
            fxLatch[(size_t) g].store (0);
            fxHeld[(size_t) g] = 0;
            fxStepHeld[(size_t) g] = 0;
        }
        macroMidiHeld = 0;
        macroMidiLatch.store (0);

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
        seqFillNow    = bp.seqFill;

        // ---- Launched sequences (A6) run on the same clock as the slice sequencer.
        if (clockPlaying != clockWasPlaying)
        {
            // Live started or stopped: launched sequences end (the clock changes domain).
            for (int p = 0; p < kNumPads; ++p) if (laneRun[(size_t) p].running) stopLane (p);
        }
        else if (clockPlaying && t.jumped)
        {
            // Loop-brace wrap / relocate: launched sequences follow Live's playhead to
            // the new place (the step under it); pending stops / restarts keep their distance.
            const double delta = clock0 - freePpq;
            for (int p = 0; p < kNumPads; ++p)
            {
                auto& r = laneRun[(size_t) p];
                if (! r.running) continue;
                const bool fromTop = std::abs (std::remainder (r.anchor, laneLength (p) * seqStepPpq)) > 1.0e-6;   // a FROM STEP 1 hand-off
                r.start += delta; r.nextStart += delta;
                if (r.endPpq < 1.0e299) r.endPpq += delta;
                r.anchor     = fromTop ? r.anchor + delta : gridAnchor (p, clock0);
                r.nextAnchor = gridAnchor (p, r.nextStart);
            }
        }
        clockWasPlaying = clockPlaying;
        for (int p = 0; p < kNumPads; ++p)
            if (laneRun[(size_t) p].running) addLaneEvents (p, 0);

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

        // (A9: the sequencer follows Live's transport only. The old RUN key /
        //  "Sequencer Run" parameter is still loaded with sets but no longer gates it.)
        juce::ignoreUnused (bp.seqRun);

        auto& data = bridge.seq;
        const int64_t kFirst = (int64_t) std::floor (p0 / seqStepPpq) - 1;     // micro-timing can pull a step earlier
        const int64_t kLast  = (int64_t) std::ceil  (p1 / seqStepPpq) + 1;
        const StepCtx ctx { p0, p1, p0, t.ppqPerSample, seqStepPpq, numSamples, bp.seqFill };

        for (int64_t k = kFirst; k <= kLast; ++k)
        {
            const int s = mod (k);
            for (int pad = 0; pad < kNumPads; ++pad)
            {
                if (isSeqPad (pad)) continue;                 // SEQ pads play only when launched
                const auto st = data.get (pad, s);
                if (! st.on || k == seqSkipOnce[(size_t) pad]) continue;
                emitStep (ctx, pad, st, (double) k * seqStepPpq, pass (k), pass (k) == seqFirstPass,
                          k, seqRollStep[(size_t) pad], seqRollPlays[(size_t) pad], 1.0f, -1, 0);
            }
        }
    }

    void AudioEngine::emitStep (const StepCtx& c, int pad, const SeqStep& st, double stepStart, int64_t passNo, bool firstPass,
                                int64_t rollKey, int64_t& rollStep, bool& rollPlays, float velScale, int owner, uint32_t ownerGen) noexcept
    {
        auto inBlock  = [&c] (double ppq) { return ppq >= c.p0 - 1.0e-9 && ppq < c.p1 - 1.0e-9; };
        auto offsetOf = [&c] (double ppq) { return juce::jlimit (0, c.numSamples - 1, (int) std::lround ((ppq - c.base) / c.rate)); };

        const double when = stepStart + st.micro / 128.0 * c.stepPpq;
        const double end  = when + c.stepPpq;
        const int r = juce::jlimit (1, 4, st.ratchet);
        const double sub = c.stepPpq / r;

        // Anything of this step in the block? (first hit, a ratchet, the FX release)
        bool touches = st.fx > 0 && inBlock (end);
        for (int j = 0; j < r && ! touches; ++j) touches = inBlock (when + j * sub);
        if (! touches) return;

        // CONDITION and CHANCE, decided once when the step's first hit comes due;
        // ratchets follow it.
        if (inBlock (when))
        {
            bool plays = seqConditionPasses (st.cond, passNo, c.fill, firstPass);
            if (plays && st.probability < 100) plays = seqRandom.nextInt (100) < st.probability;
            rollStep  = rollKey;
            rollPlays = plays;
        }
        if (rollStep != rollKey || ! rollPlays) return;

        const uint64_t packed = st.pack();
        const float vel = juce::jlimit (0.0f, 1.0f, (float) st.velocity / 127.0f * velScale);
        for (int j = 0; j < r && st.hit; ++j)
            if (inBlock (when + j * sub))
                insertEvent ({ offsetOf (when + j * sub), pad, vel, SeqKind::padHit, packed, 0, owner, ownerGen });

        // FX: held on the pad's group for the length of the step (the release is never dropped).
        if (st.fx > 0)
        {
            const int g = padGroup[(size_t) pad];
            uint64_t endBits;
            std::memcpy (&endBits, &end, sizeof end);
            if (inBlock (when)) insertEvent ({ offsetOf (when), g, 0.0f, SeqKind::stepFxOn,  endBits, (uint32_t) (st.fx - 1), owner, ownerGen });
            if (inBlock (end))  insertEvent ({ offsetOf (end),  g, 0.0f, SeqKind::stepFxOff, endBits, (uint32_t) (st.fx - 1) });
        }
    }

    //==========================================================================
    //  Sequence pads (A6)
    //==========================================================================
    int AudioEngine::laneLength (int pad) const noexcept
    {
        const int own = padSeqLen[(size_t) pad];
        return own > 0 ? own : seqLengthNow;
    }

    double AudioEngine::launchBoundary (int pad, int sampleOffset) const noexcept
    {
        const double c = clockAt (sampleOffset);
        double q = 0.0, origin = 0.0;
        switch (padLaunch[(size_t) pad])
        {
            case params::launchNow:       return c;
            case params::launchSixteenth: q = 0.25; break;
            case params::launchBeat:      q = 1.0;  break;
            default:
                q = 4.0;
                if (clockPlaying && transportNow != nullptr) { q = transportNow->ppqPerBar(); origin = transportNow->ppqOfBarStart; }
                break;
        }
        return origin + std::ceil ((c - origin) / q - 1.0e-9) * q;
    }

    double AudioEngine::gridAnchor (int pad, double ppq) const noexcept
    {
        const double len = juce::jmax (1, laneLength (pad)) * seqStepPpq;
        return std::floor ((ppq + 1.0e-9) / len) * len;
    }

    void AudioEngine::launchLane (int pad, int sampleOffset, double anchor, double start, float hitVel) noexcept
    {
        auto& r = laneRun[(size_t) pad];
        const uint32_t gen = r.gen + 1;
        r = {};
        r.gen     = gen;
        r.running = true;
        r.once    = padMode[(size_t) pad] == params::modeSeqOne;
        r.anchor  = anchor;
        r.start   = start;
        r.hitVel  = juce::jlimit (0.0f, 1.0f, hitVel);
        addLaneEvents (pad, sampleOffset);
    }

    void AudioEngine::hitSeqPad (int pad, float velocity, int sampleOffset) noexcept
    {
        auto& r = laneRun[(size_t) pad];

        // Live stopped and nothing else on the free clock: start it here, so the
        // pattern's grid (and its step 1) begins with this hit.
        if (! clockPlaying && ! r.running)
        {
            bool others = false;
            for (int p = 0; p < kNumPads; ++p)
                others = others || laneRun[(size_t) p].running || sliceRun[(size_t) p].running;
            if (! others) clock0 = -sampleOffset * clockDppq;
        }

        const double b = launchBoundary (pad, sampleOffset);
        if (! r.running)
        {
            launchLane (pad, sampleOffset, gridAnchor (pad, b), b, velocity);
            return;
        }
        if (r.once)
        {
            // SEQ ONE again: another full pass from the next boundary (still in time with the playhead).
            r.hasNext = true; r.nextStart = b; r.nextAnchor = gridAnchor (pad, b); r.endPpq = b; r.releaseAtEnd = false;
            r.hitVel = juce::jlimit (0.0f, 1.0f, velocity);
        }
        else if (r.releaseAtEnd)
        {
            r.endPpq = 1.0e300; r.releaseAtEnd = false;        // a stop was waiting: cancel it
        }
        else
        {
            r.endPpq = b; r.releaseAtEnd = true;               // SEQ LOOP: stop at the next boundary
        }
        ++r.gen;
        addLaneEvents (pad, sampleOffset);
    }

    void AudioEngine::stopLane (int pad, float fadeMs) noexcept
    {
        auto& r = laneRun[(size_t) pad];
        r.running = false;
        r.hasNext = false;
        ++r.gen;
        for (auto& v : voices[(size_t) pad]) v.release (fadeMs);
        bridge.laneStep[(size_t) pad].store (-1, std::memory_order_relaxed);
    }

    void AudioEngine::addLaneEvents (int pad, int fromOffset) noexcept
    {
        auto& r = laneRun[(size_t) pad];
        if (! r.running || clockDppq <= 0.0) return;
        const double p0 = clockAt (fromOffset), p1 = clockAt (blockLength), step = seqStepPpq;
        const int L = laneLength (pad);
        const StepCtx ctx { p0, p1, clock0, clockDppq, step, blockLength, seqFillNow };
        auto& data = bridge.seq;

        // A segment: steps on the grid from `a`, playing from `from` (the launch) until `e`.
        // SEQ ONE plays one full pass (LEN steps) from its launch, in time with the playhead.
        auto segment = [&] (double a, double from, double e)
        {
            const double cut = r.once ? juce::jmin (e, from + L * step) : e;
            const int64_t iFirst = juce::jmax ((int64_t) 0, (int64_t) std::floor ((juce::jmax (p0, from) - a) / step) - 1);
            const int64_t iLast  = (int64_t) std::ceil ((p1 - a) / step) + 1;
            for (int64_t i = iFirst; i <= iLast; ++i)
            {
                const double stepStart = a + (double) i * step;
                if (stepStart >= cut - 1.0e-9) break;
                if (stepStart < from - 1.0e-9) continue;              // before the launch
                const auto st = data.get (pad, (int) (i % L));
                if (! st.on) continue;
                // Passes count on the grid (like the free lanes); FIRST = the first time round after the launch.
                emitStep (ctx, pad, st, stepStart, i / L, stepStart < from + L * step - 1.0e-9, (int64_t) std::llround (stepStart * 1024.0),
                          r.rollKey, r.rollPlays, r.hitVel, pad, r.gen);
            }
        };
        segment (r.anchor, r.start, r.endPpq);
        if (r.hasNext) segment (r.nextAnchor, r.nextStart, 1.0e300);

        // A stop cuts the sound right there.
        if (r.releaseAtEnd && r.endPpq >= p0 - 1.0e-9 && r.endPpq < p1 - 1.0e-9)
            insertEvent ({ juce::jlimit (0, blockLength - 1, (int) std::lround ((r.endPpq - clock0) / clockDppq)),
                           pad, 0.0f, SeqKind::laneStop, 0, 0, pad, r.gen });
    }

    void AudioEngine::advanceLanes() noexcept
    {
        const double c1 = clockAt (blockLength);
        for (int p = 0; p < kNumPads; ++p)
        {
            auto& r = laneRun[(size_t) p];
            int step = -1;
            bool queued = false;
            if (r.running)
            {
                const double len = laneLength (p) * seqStepPpq;
                if (r.hasNext && c1 > r.nextStart + 1.0e-9)     { r.anchor = r.nextAnchor; r.start = r.nextStart; r.hasNext = false; r.endPpq = 1.0e300; }
                else if (! r.hasNext && c1 > r.endPpq + 1.0e-9) { r.running = false; }
                if (r.running && r.once && ! r.hasNext && c1 > r.start + len + 1.0e-9) r.running = false;
            }
            if (r.running && seqStepPpq > 0.0)
            {
                const double at = c1 - 1.0e-9;
                if (at >= r.anchor)
                    step = (int) ((int64_t) std::floor ((at - r.anchor) / seqStepPpq) % juce::jmax (1, laneLength (p)));
                queued = at < r.start || r.hasNext || r.releaseAtEnd;
                if (at < r.start) step = -1;
            }
            bridge.laneStep[(size_t) p].store (step, std::memory_order_relaxed);
            laneQueued[(size_t) p] = queued;
        }
    }

    void AudioEngine::applyFx (int g) noexcept
    {
        uint32_t mask = (fxParamMask[(size_t) g] ^ fxLatch[(size_t) g].load (std::memory_order_relaxed))
                        | fxHeld[(size_t) g] | fxStepHeld[(size_t) g];
        auto& params = appliedFxParams[(size_t) g];
        params = fxParams[(size_t) g];

        // Macros on this group: their effects on, their recipe's settings
        // (a later macro wins a shared setting). Off again = your own settings.
        const uint32_t macros = macroActiveMask();
        if (macros != 0 && (macroGroupNow == 4 || macroGroupNow == g))
            for (int m = 0; m < macro::kNumMacros; ++m)
                if ((macros >> m) & 1u)
                {
                    const auto& d = macro::def (m);
                    for (int i = 0; i < d.numSettings; ++i)
                    {
                        const auto& s = d.settings[i];
                        mask |= 1u << s.fx;
                        if (s.param >= 0) params[(size_t) s.fx][(size_t) s.param] = macro::valueAt (s, macroAmountNow);
                    }
                }
        groups[(size_t) g].setFx (mask, params);
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

    void AudioEngine::recordHit (int pad, float velocity, int sampleOffset, int slice) noexcept
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
        if (slice >= 0) st.slice = juce::jlimit (0, 16, slice);    // A9: a slice played on the Mikro records as that slice
        bridge.seq.set (pad, idx, st);

        // That step may still be ahead in this pass (hit a little early):
        // you've just played it, so the sequencer mustn't play it again now.
        seqSkipOnce[(size_t) pad] = k;
    }

    bool AudioEngine::handleFxNote (int note, bool on) noexcept
    {
        // Macros 1..8 on E4..B4: held, or toggled with MACRO LATCH.
        const int m = note - macro::kFirstMidiNote;
        if (juce::isPositiveAndBelow (m, macro::kNumMacros))
        {
            const uint32_t bit = 1u << m;
            if (macroLatchNow) { if (on) macroMidiLatch.fetch_xor (bit, std::memory_order_relaxed); }
            else               { if (on) macroMidiHeld |= bit; else macroMidiHeld &= ~bit; }
            for (int g = 0; g < kNumGroups; ++g) applyFx (g);
            return true;
        }

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
            const int tapGroup = rsTapActive ? rsSource.load (std::memory_order_relaxed) - 1 : -1;
            for (int g = 0; g < kNumGroups; ++g)
                groupPeak[(size_t) g] = juce::jmax (groupPeak[(size_t) g],
                                                    groups[(size_t) g].mixInto (buffer, start, n, clock, g == tapGroup ? &rsTap : nullptr));

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

        // ---- A7: Push 3 pad pressure -> each pad's voices (smoothed per block).
        {
            const double bpm = transport.bpm > 0.0 ? transport.bpm : 120.0;
            const double sixteenth = sampleRate * 60.0 / bpm * 0.25;
            for (int p = 0; p < kNumPads; ++p)
            {
                const float target = juce::jlimit (0.0f, 1.0f, bridge.padPressure[(size_t) p].load (std::memory_order_relaxed));
                auto& sm = pressNow[(size_t) p];
                sm += (target - sm) * 0.5f;
                if (sm < 1.0e-4f) sm = 0.0f;
                const int what = params.pads[(size_t) p].press;
                for (auto& v : voices[(size_t) p])
                    if (v.isActive()) v.setPressure (sm, what, sixteenth);
            }
        }

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

        // ---- RESAMPLE from a group: its output is gathered while rendering.
        {
            const int st = rsState.load (std::memory_order_relaxed);
            rsTapActive = (st == rsArmed || st == rsRecording) && rsSource.load (std::memory_order_relaxed) > 0
                          && numSamples <= rsTap.getNumSamples();
            if (rsTapActive) rsTap.clear (0, numSamples);
        }

        // ---- Sequencer steps due in this block (sample positions, sorted).
        collectSequencerEvents (transport, params, numSamples);

        // ---- Mouse pad presses from the UI land at the start of the block.
        PadUiEvent ev;
        while (bridge.padEvents.pop (ev))
        {
            if (ev.noteOn) { recordHit (ev.pad, ev.velocity, 0); triggerPad (ev.pad, ev.velocity, 0); }
            else           releasePad (ev.pad);
        }
        // ---- A8: the Mikro MK3 (pads, slices, note-repeat rolls).
        {
            ControllerHit h;
            while (bridge.mikroEvents.pop (h))
            {
                const int p = h.pad;
                if (p < 0 || p >= kNumPads) continue;
                switch (h.kind)
                {
                    case ControllerHit::padOn:     pressNow[(size_t) p] = 0.0f; recordHit (p, h.velocity, 0); triggerPad (p, h.velocity, 0); break;
                    case ControllerHit::padOff: releasePad (p); break;
                    case ControllerHit::sliceOn:
                        recordHit (p, h.velocity, 0, h.slice);          // A9: armed lane + Live playing: written with its slice
                        controllerSliceHit (p, h.slice, h.velocity, 0);
                        break;
                    case ControllerHit::rollOn:
                        // A press that just played (velocity > 0 = it came with the hit) skips grid points in the next 10 ms.
                        rollRun[(size_t) p] = { true, juce::jlimit (0, 2, (int) h.rate), juce::jmax (0.01f, std::abs (h.velocity)), h.slice,
                                                h.velocity > 0.0f ? clockAt (0) + 0.010 * sampleRate * clockDppq : -1.0e300 };
                        break;
                    case ControllerHit::rollOff: rollRun[(size_t) p].active = false; break;
                }
            }
            addRollEvents (numSamples);
        }

        // ---- ...and pads played on a controller's own port (A7: Push 3 User mode).
        while (bridge.controllerPadEvents.pop (ev))
        {
            if (ev.pad < 0 || ev.pad >= kNumPads) continue;
            if (ev.noteOn) pressNow[(size_t) ev.pad] = 0.0f;       // a fresh hit starts without the last press's pressure
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
                renderVoices (buffer, pos, e.offset - pos); // (a SWAP hand-off can fire in here)
                pos = e.offset;
                currentOffset = e.offset;
                if (e.owner >= 0 && laneRun[(size_t) e.owner].gen != e.ownerGen) continue;   // that sequence was stopped / relaunched
                switch (e.kind)
                {
                    case SeqKind::bankSwitch:
                        switchBank (e.pad);
                        break;
                    case SeqKind::laneStop:
                        stopLane (e.pad);
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
                    case SeqKind::rollHit:
                        if (rollRun[(size_t) e.pad].active) controllerSliceHit (e.pad, (int) e.step, e.velocity, e.offset);
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
        advanceLanes();
        freePpq = clock0 + numSamples * clockDppq;

        transportNow = nullptr;
        currentParams = nullptr;

        // ---- Master
        master.setGainDb (params.masterGainDb);
        master.setLimiterEnabled (params.limiterEnabled);
        const auto result = master.process (buffer, numSamples);
        resampleCapture (buffer, transport, numSamples);

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
        bridge.macroActive.store (macroActiveMask(), std::memory_order_relaxed);
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
            queued = queued || laneQueued[(size_t) p];
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
    //==========================================================================
    //  RESAMPLE
    //==========================================================================
    bool AudioEngine::armResample (int source, int bars, bool atBar) noexcept
    {
        const int st = rsState.load();
        if (st == rsRecording || st == rsDone) return false;
        rsSource.store (juce::jlimit (0, kNumGroups, source));
        rsBars.store (juce::jlimit (1, 64, bars));
        rsAtBar.store (atBar);
        rsCancel.store (false);
        rsWritten.store (0);
        rsState.store (rsArmed);
        return true;
    }

    void AudioEngine::resampleCapture (const juce::AudioBuffer<float>& out, const TransportState& t, int numSamples) noexcept
    {
        int st = rsState.load (std::memory_order_relaxed);
        if (st == rsIdle || st == rsDone) return;
        if (rsCancel.exchange (false)) { rsState.store (rsIdle); return; }

        int from = 0;
        if (st == rsArmed)
        {
            const bool playing = t.isPlaying && t.ppqPerSample > 0.0;
            const double barPpq = t.ppqPerBar();
            const int bars = rsBars.load (std::memory_order_relaxed);
            if (playing && rsAtBar.load (std::memory_order_relaxed))
            {
                const double p0 = t.ppqAtBlockStart;
                const double next = t.ppqOfBarStart + std::ceil ((p0 - t.ppqOfBarStart) / barPpq - 1.0e-9) * barPpq;
                from = (int) std::lround ((next - p0) / t.ppqPerSample);
                if (from >= numSamples) return;                 // the bar line is in a later block
                from = juce::jmax (0, from);
            }
            const double samples = playing ? bars * barPpq / t.ppqPerSample
                                           : bars * barPpq * 60.0 / juce::jmax (1.0, t.bpm) * sampleRate;
            rsLength.store (juce::jlimit (1, rsBuffer.getNumSamples(), (int) std::lround (samples)), std::memory_order_relaxed);
            rsWritten.store (0, std::memory_order_relaxed);
            rsState.store (rsRecording, std::memory_order_relaxed);
        }

        // Recording: what you hear (the master, after the limiter) or one group.
        const int src = rsSource.load (std::memory_order_relaxed);
        const auto& in = src == 0 ? out : rsTap;
        if (src > 0 && ! rsTapActive) return;
        const int written = rsWritten.load (std::memory_order_relaxed);
        const int n = juce::jmin (numSamples - from, rsLength.load (std::memory_order_relaxed) - written);
        if (n > 0)
        {
            for (int ch = 0; ch < 2; ++ch)
                rsBuffer.copyFrom (ch, written, in, juce::jmin (ch, in.getNumChannels() - 1), from, n);
            rsWritten.store (written + n, std::memory_order_relaxed);
        }
        if (written + juce::jmax (0, n) >= rsLength.load (std::memory_order_relaxed))
            rsState.store (rsDone, std::memory_order_release);
    }
}
