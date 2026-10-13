#include "Engine/AudioEngine.h"
#include "Core/Parameters.h"
#include <cmath>

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
            const bool valid = juce::isPositiveAndBelow (cmd.pad, kNumPads);

            if (valid && cmd.setSlices)
                padSlices[(size_t) cmd.pad] = cmd.slices;      // plain copy, no allocation

            if (valid && cmd.setSample && padSample[(size_t) cmd.pad].get() != cmd.buffer)
            {
                // Old voices fade out still holding the old buffer (kept alive by
                // their own reference); new triggers use the new one.
                for (auto& v : voices[(size_t) cmd.pad])
                    v.release();
                padSample[(size_t) cmd.pad] = cmd.buffer;            // +1
            }

            if (cmd.buffer != nullptr)
                cmd.buffer->decReferenceCountWithoutDeleting();       // adopt the sender's +1
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

        // Group buses: level, pan, mute / solo (solo is between groups).
        bool anyGroupSolo = false;
        for (const auto& gp : bp.groups) anyGroupSolo = anyGroupSolo || gp.solo;
        for (int g = 0; g < kNumGroups; ++g)
        {
            const auto& gp = bp.groups[(size_t) g];
            groups[(size_t) g].setParams (gp.levelDb, gp.pan, ! gp.mute && (! anyGroupSolo || gp.solo), gp.bypass);
        }

        for (int p = 0; p < kNumPads; ++p)
        {
            const auto& pp = bp.pads[(size_t) p];
            padMode[(size_t) p]    = pp.mode;
            padChoke[(size_t) p]   = pp.choke;
            padVelSens[(size_t) p] = juce::jlimit (0.0f, 1.0f, pp.velSens);
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

            for (auto& v : voices[(size_t) p])
                if (v.isActive() && v.getBuffer() == buf)
                    v.update (s);
        }
    }

    void AudioEngine::chokeOthers (int pad, int group) noexcept
    {
        if (group <= 0) return;
        for (int p = 0; p < kNumPads; ++p)
            if (p != pad && padChoke[(size_t) p] == group)
                for (auto& v : voices[(size_t) p])
                    v.release (3.0f);           // very short, still click-free
    }

    void AudioEngine::triggerPad (int pad, float velocity, int sampleOffset) noexcept
    {
        if (! juce::isPositiveAndBelow (pad, kNumPads)) return;

        bridge.padTriggerCount[(size_t) pad].fetch_add (1, std::memory_order_relaxed);
        padFlash[(size_t) pad] = true;     // lamp blinks even on an empty pad

        chokeOthers (pad, padChoke[(size_t) pad]);

        // Velocity sensitivity: 0 = every hit at full level, 1 = level follows velocity.
        const float sens  = padVelSens[(size_t) pad];
        const float level = 1.0f - sens + sens * velocity;

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
                startVoice (pad, level, sampleOffset);
                return;
            }

            if (pending.remaining <= 0) fireSwap (group, sampleOffset);
            return;
        }

        if (padSample[(size_t) pad] == nullptr) return;
        startVoice (pad, level, sampleOffset);
    }

    void AudioEngine::startVoice (int pad, float level, int sampleOffset, double startAt) noexcept
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
        slots[(size_t) cur].start (sample, padSettings[(size_t) pad], level, ppqHere, playing, startAt);
    }

    //==========================================================================
    //  SWAP groups
    //==========================================================================
    int AudioEngine::swapOwner (int group) const noexcept
    {
        const int p = swapPlaying[(size_t) group];
        if (p < 0 || padSwap[(size_t) p] != group) return -1;
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
            if (owner >= 0)
                for (auto& v : voices[(size_t) owner]) v.release (3.0f);
            swapPlaying[(size_t) group] = -1;
            return;
        }

        const int to = pending.pad;
        if (padSample[(size_t) to] == nullptr) return;

        const double at = handoffPosition (owner, to, sampleOffset);

        // The outgoing break was cut on its own slice point (1 ms before its
        // next hit), so a 1 ms fade is clean and nothing of that hit leaks.
        if (owner >= 0 && owner != to)
            for (auto& v : voices[(size_t) owner]) v.release (1.0f);

        swapPlaying[(size_t) group] = to;
        padFlash[(size_t) to] = true;
        startVoice (to, pending.level, sampleOffset, at);
    }

    void AudioEngine::releasePad (int pad) noexcept
    {
        // One-shots (and slices) ignore note-off; loops play while held.
        // Pads in a SWAP group latch: they play until another pad takes over.
        if (! juce::isPositiveAndBelow (pad, kNumPads) || padMode[(size_t) pad] != params::modeLoop
             || padSwap[(size_t) pad] > 0) return;
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
            if (pad >= 0) triggerPad (pad, m.getFloatVelocity(), currentOffset);
        }
        else if (m.isNoteOff())
        {
            const int pad = padForNote (m.getNoteNumber());
            if (pad >= 0) releasePad (pad);
        }
        else if (m.isAllSoundOff())   // CC 120: a hardware panic button
        {
            panic();
        }
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

            for (int g = 0; g < kNumGroups; ++g)
                groupPeak[(size_t) g] = juce::jmax (groupPeak[(size_t) g], groups[(size_t) g].mixInto (buffer, start, n));

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

        // ---- Mouse pad presses from the UI land at the start of the block.
        PadUiEvent ev;
        while (bridge.padEvents.pop (ev))
        {
            if (ev.noteOn) triggerPad (ev.pad, ev.velocity, 0);
            else           releasePad (ev.pad);
        }

        // ---- Sample-accurate MIDI: render up to each event, then apply it.
        int pos = 0;
        for (const auto meta : midi)
        {
            const int eventPos = juce::jlimit (0, numSamples, meta.samplePosition);
            renderVoices (buffer, pos, eventPos - pos);
            pos = eventPos;
            currentOffset = eventPos;
            handleMidi (meta.getMessage());
        }
        renderVoices (buffer, pos, numSamples - pos);

        transportNow = nullptr;

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
            storeMax (bridge.groupPeak[(size_t) g], groupPeak[(size_t) g]);
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
