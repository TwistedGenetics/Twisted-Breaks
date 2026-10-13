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
        padPeak.fill (0.0f);
        padFlash.fill (false);
        master.reset();
        scopeCounter = 0;
    }

    //==========================================================================
    bool AudioEngine::postSample (int pad, SampleBuffer* buffer) noexcept
    {
        return sampleCommands.push ({ pad, buffer });
    }

    void AudioEngine::drainSampleQueue() noexcept
    {
        // Not the audio thread (audio has stopped), so freeing here is fine.
        SampleCommand cmd;
        while (sampleCommands.pop (cmd))
            if (cmd.buffer != nullptr && cmd.buffer->decReferenceCountWithoutDeleting())
                delete cmd.buffer;
    }

    void AudioEngine::applySampleCommands() noexcept
    {
        SampleCommand cmd;
        while (sampleCommands.pop (cmd))
        {
            if (! juce::isPositiveAndBelow (cmd.pad, kNumPads))
            {
                if (cmd.buffer != nullptr) cmd.buffer->decReferenceCountWithoutDeleting();
                continue;
            }

            // Old voices fade out still holding the old buffer (kept alive by
            // their own reference); new triggers use the new one.
            for (auto& v : voices[(size_t) cmd.pad])
                v.release();

            padSample[(size_t) cmd.pad] = cmd.buffer;            // +1
            if (cmd.buffer != nullptr)
                cmd.buffer->decReferenceCountWithoutDeleting();   // adopt the sender's +1
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

        for (int p = 0; p < kNumPads; ++p)
        {
            const auto& pp = bp.pads[(size_t) p];
            padMode[(size_t) p]    = pp.mode;
            padChoke[(size_t) p]   = pp.choke;
            padVelSens[(size_t) p] = juce::jlimit (0.0f, 1.0f, pp.velSens);

            auto& s = padSettings[(size_t) p];
            const auto* buf = padSample[(size_t) p].get();
            if (buf == nullptr) continue;

            const int len = buf->getNumSamples();

            // Region from START / END. If automation crosses them, use the span between.
            float a = juce::jlimit (0.0f, 1.0f, pp.start);
            float b = juce::jlimit (0.0f, 1.0f, pp.end);
            if (b < a) std::swap (a, b);
            int s0 = (int) std::lround ((double) a * len);
            int s1 = (int) std::lround ((double) b * len);
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
            const float pan = juce::jlimit (-1.0f, 1.0f, pp.pan);
            s.gainLeft  = g * (pan > 0.0f ? 1.0f - pan : 1.0f);
            s.gainRight = g * (pan < 0.0f ? 1.0f + pan : 1.0f);

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

        const auto& sample = padSample[(size_t) pad];
        if (sample == nullptr) return;

        // Velocity sensitivity: 0 = every hit at full level, 1 = level follows velocity.
        const float sens  = padVelSens[(size_t) pad];
        const float level = 1.0f - sens + sens * velocity;

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
        slots[(size_t) cur].start (sample, padSettings[(size_t) pad], level, ppqHere, playing);
    }

    void AudioEngine::releasePad (int pad) noexcept
    {
        // One-shots (and slices) ignore note-off; loops play while held.
        if (! juce::isPositiveAndBelow (pad, kNumPads) || padMode[(size_t) pad] != params::modeLoop) return;
        for (auto& v : voices[(size_t) pad])
            if (v.isLooping()) v.release();
    }

    void AudioEngine::panic() noexcept
    {
        for (auto& padVoices : voices)
            for (auto& v : padVoices)
                v.release();       // 5 ms fade, never a hard cut

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
        if (num <= 0) return;

        for (int p = 0; p < kNumPads; ++p)
            for (auto& v : voices[(size_t) p])
                if (v.isActive())
                    padPeak[(size_t) p] = juce::jmax (padPeak[(size_t) p], v.renderAdding (buffer, start, num));
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

        // ---- New / cleared samples from the loader, then this block's pad settings.
        transportNow  = &transport;
        currentOffset = 0;
        applySampleCommands();
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
