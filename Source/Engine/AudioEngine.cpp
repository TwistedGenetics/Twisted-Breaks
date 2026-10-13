#include "Engine/AudioEngine.h"
#include <cmath>

namespace tb
{
    AudioEngine::AudioEngine (EngineBridge& b) : bridge (b)
    {
        for (int p = 0; p < kNumPads; ++p)
            padNote[(size_t) p].store (kDefaultFirstNote + p);
    }

    void AudioEngine::prepare (double newSampleRate, int maxBlockSize)
    {
        sampleRate = newSampleRate;

        for (int p = 0; p < kNumPads; ++p)
            for (auto& v : voices[(size_t) p])
            {
                v.prepare (sampleRate);
                v.configureForPad (p);
                v.kill();
            }

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
        master.reset();
        scopeCounter = 0;
    }

    //==========================================================================
    void AudioEngine::setPadNote (int pad, int note) noexcept
    {
        if (juce::isPositiveAndBelow (pad, kNumPads))
            padNote[(size_t) pad].store (juce::jlimit (-1, 127, note));
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
    void AudioEngine::triggerPad (int pad, float velocity) noexcept
    {
        if (! juce::isPositiveAndBelow (pad, kNumPads)) return;

        // Retrigger: the sounding voice fades out over 5 ms while the next
        // slot starts fresh — no click, no cut-off tail artefact.
        auto& slots = voices[(size_t) pad];
        auto& cur   = currentVoice[(size_t) pad];

        slots[(size_t) cur].release();
        cur = (cur + 1) % kVoicesPerPad;
        slots[(size_t) cur].start (velocity);

        bridge.padTriggerCount[(size_t) pad].fetch_add (1, std::memory_order_relaxed);
    }

    void AudioEngine::releasePad (int) noexcept
    {
        // One-shot calibration voices ignore note-off. From Phase 2 the
        // pad's playback mode decides (gate/loop modes release here).
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

            const int pad = padForNote (m.getNoteNumber());
            if (pad >= 0) triggerPad (pad, m.getFloatVelocity());
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

        // ---- Panic: UI button, automatable parameter rising edge, or the
        //      host stopping isn't a panic (tails ring out naturally).
        const bool panicEdge = params.panicParam && ! lastPanicParam;
        lastPanicParam = params.panicParam;
        if (bridge.panicRequested.exchange (false) || panicEdge)
            panic();

        // ---- Mouse pad presses from the UI land at the start of the block.
        PadUiEvent ev;
        while (bridge.padEvents.pop (ev))
        {
            if (ev.noteOn) triggerPad (ev.pad, ev.velocity);
            else           releasePad (ev.pad);
        }

        // ---- Sample-accurate MIDI: render up to each event, then apply it.
        int pos = 0;
        for (const auto meta : midi)
        {
            const int eventPos = juce::jlimit (0, numSamples, meta.samplePosition);
            renderVoices (buffer, pos, eventPos - pos);
            pos = eventPos;
            handleMidi (meta.getMessage());
        }
        renderVoices (buffer, pos, numSamples - pos);

        juce::ignoreUnused (transport);   // consumed by the sequencer from Phase 7

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
            const float decayed = a.load (std::memory_order_relaxed) * decay;
            a.store (juce::jmax (decayed, juce::jmin (1.0f, padPeak[(size_t) p] * 2.5f)),
                     std::memory_order_relaxed);
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
