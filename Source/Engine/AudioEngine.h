#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <vector>
#include "Core/EngineBridge.h"
#include "Core/TransportSync.h"
#include "Engine/PadVoice.h"
#include "Engine/SampleBuffer.h"
#include "Engine/MasterSection.h"

namespace tb
{
    /*
        AudioEngine
        -----------
        Owns all real-time DSP. PluginProcessor::processBlock() is a thin
        wrapper that snapshots parameters and calls AudioEngine::process().

        Signal flow (Phase 2):
            MIDI / UI pad events -> per-pad PadVoices -> master -> output
        Signal flow (from Phase 5):
            voices -> GroupBus A..D -> FXChain per group -> master -> output

        Real-time rules enforced here:
          * every buffer is allocated in prepare(), never in process()
          * no locks; UI communication only through EngineBridge
          * samples arrive through a lock-free queue (postSample) already
            decoded; the engine never touches files
          * MIDI is rendered sample-accurately by splitting the block at
            each event's sample position
    */
    class AudioEngine
    {
    public:
        static constexpr int kVoicesPerPad = 2;  // current + one fading out on retrigger

        explicit AudioEngine (EngineBridge& bridge);
        ~AudioEngine();

        void prepare (double sampleRate, int maxBlockSize);
        void reset() noexcept;

        struct PadParams
        {
            float gainDb  = 0.0f;
            float pan     = 0.0f;
            float pitch   = 0.0f;
            float start   = 0.0f;
            float end     = 1.0f;
            bool  reverse = false;
        };

        struct BlockParams
        {
            float masterGainDb   = 0.0f;
            bool  limiterEnabled = true;
            bool  panicParam     = false;
            std::array<PadParams, kNumPads> pads {};
        };

        void process (juce::AudioBuffer<float>& buffer,
                      const juce::MidiBuffer& midi,
                      const TransportState& transport,
                      const BlockParams& params) noexcept;

        /** Stops every voice with a short fade. Safe on the audio thread. */
        void panic() noexcept;

        // ---- Sample hand-off (message thread -> audio thread) ----------------
        /**
            Queues `buffer` (may be nullptr = clear) as pad's new sample. The
            caller must have added one reference that the engine adopts.
            Returns false if the queue is full (caller retries later).
        */
        bool postSample (int pad, SampleBuffer* bufferWithExtraRef) noexcept;

        /** Message thread, audio stopped: drop anything still queued. */
        void drainSampleQueue() noexcept;

        // ---- MIDI note map (pads default to notes 36..51) ---------------------
        void setPadNote (int pad, int note) noexcept;
        int  getPadNote (int pad) const noexcept;
        int  padForNote (int note) const noexcept;   // -1 if unmapped

        static constexpr int kDefaultFirstNote = 36;  // C1 in Ableton = Drum Rack pad 1

    private:
        void triggerPad (int pad, float velocity) noexcept;
        void releasePad (int pad) noexcept;
        void handleMidi (const juce::MidiMessage& m) noexcept;
        void renderVoices (juce::AudioBuffer<float>& buffer, int start, int num) noexcept;
        void applySampleCommands() noexcept;
        void computeVoiceSettings (const BlockParams&) noexcept;

        EngineBridge& bridge;
        MasterSection master;

        struct SampleCommand { int pad = 0; SampleBuffer* buffer = nullptr; };
        SpscQueue<SampleCommand, 64> sampleCommands;

        std::array<SampleBuffer::Ptr, kNumPads> padSample;           // audio-thread owned
        std::array<PadVoice::Settings, kNumPads> padSettings {};     // this block's settings
        std::array<std::array<PadVoice, kVoicesPerPad>, kNumPads> voices;
        std::array<int, kNumPads>   currentVoice {};
        std::array<float, kNumPads> padPeak {};       // per-block peak for lamps
        std::array<bool, kNumPads>  padFlash {};      // triggered this block
        std::array<std::atomic<int>, kNumPads> padNote {};

        bool   lastPanicParam = false;
        double sampleRate     = 44100.0;

        // Scope decimation
        int   scopeDecimation = 8;
        int   scopeCounter    = 0;
        std::vector<float> scopeScratch;              // sized in prepare()
    };
}
